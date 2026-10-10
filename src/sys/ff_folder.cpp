// FatFs's public API on a host folder: the virtual SD card.
//
// Firmware calls the FatFs functions it always did (f_open, f_read, f_stat, ...), and they act on
// the files in the card folder (board/sd_card.h). The behaviour follows FatFs R0.12c, as libDaisy
// configures it (sys/ffconf.h), wherever a folder can show it: names are matched without regard
// to case, a rename doesn't replace an existing file, a seek past the end of a file open for
// writing extends it, and the string functions turn "\n" into "\r\n" and back. What a folder
// can't show (FAT allocation, cluster sizes, a write cut off halfway) isn't modelled.
//
// These run on the firmware's thread, sometimes inside an interrupt handler, so they only use
// async-signal-safe system calls and fixed buffers: no allocation, no locks, no stdio.
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio> // renameat, renameat2
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "board/sd_card.h"
#include "diskio.h"
#include "ff.h"

namespace
{
using daisycola::sd::BusyScope;

constexpr int    kMaxFiles   = 64; // files open at once
constexpr int    kMaxDirs    = 16; // directories open at once
constexpr size_t kPathMax    = 1024;
constexpr size_t kNameMax    = _MAX_LFN;
constexpr WORD   kClusterSec = 64; // a 32 KB cluster, as on a large FAT32 card
constexpr DWORD  kMaxSize    = 0xFFFFFFFFu;

// ---- State ------------------------------------------------------------------------------------

struct FileSlot
{
    std::atomic<FIL*> fil{nullptr};
    int               fd = -1;
};

struct DirSlot
{
    std::atomic<DIR*> dir{nullptr};
    int               fd   = -1;
    size_t            pos  = 0; // next entry in buf
    size_t            len  = 0; // bytes in buf
    bool              find = false;
    char              pattern[kNameMax + 1];
    alignas(8) char   buf[4096];
};

FileSlot files[kMaxFiles];
DirSlot  dirs[kMaxDirs];

std::atomic<FATFS*> mounted{nullptr};
WORD                mount_id = 0;

// The current directory (f_chdir), relative to the card root: "" or "a/b".
char cwd[kPathMax] = "";

// Directory entries, read with the getdents64 system call: <dirent.h> can't be included, since
// its DIR clashes with FatFs's, and readdir allocates.
struct Dirent64
{
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

ssize_t ReadEntries(int fd, char* buf, size_t size)
{
    return syscall(SYS_getdents64, fd, buf, size);
}

int Card()
{
    return daisycola::sd::FolderFd();
}

FRESULT FromErrno(int err)
{
    switch(err)
    {
        case ENOENT: return FR_NO_FILE;
        case ENOTDIR: return FR_NO_PATH;
        case EEXIST:
        case ENOTEMPTY: return FR_EXIST;
        case EACCES:
        case EPERM:
        case EISDIR:
        case EROFS:
        case ENOSPC: // FatFs reports a full card (or directory) as access denied
        case EDQUOT:
        case EBUSY: return FR_DENIED;
        case ENAMETOOLONG:
        case EINVAL: return FR_INVALID_NAME;
        case EMFILE:
        case ENFILE: return FR_TOO_MANY_OPEN_FILES;
        default: return FR_DISK_ERR;
    }
}

// FatFs's own message for calls it can't do here, written with write(2): this may be inside an
// interrupt handler.
void Unsupported(const char* what)
{
    static const char prefix[] = "daisycola: ";
    static const char suffix[] = " is not supported on a folder SD card\n";
    ssize_t           ignored  = write(STDERR_FILENO, prefix, sizeof prefix - 1);
    ignored                    = write(STDERR_FILENO, what, std::strlen(what));
    ignored                    = write(STDERR_FILENO, suffix, sizeof suffix - 1);
    (void)ignored;
}

bool CopyString(char* to, size_t size, const char* from)
{
    const size_t n = std::strlen(from);
    if(n >= size)
        return false;
    std::memcpy(to, from, n + 1);
    return true;
}

char Upper(char c)
{
    return c >= 'a' && c <= 'z' ? char(c - 0x20) : c;
}

bool SameName(const char* a, const char* b)
{
    for(; *a && *b; a++, b++)
        if(Upper(*a) != Upper(*b))
            return false;
    return *a == *b;
}

// FatFs's pattern matching: '?' is any character, '*' any run of them, case ignored.
bool Matches(const char* pat, const char* name)
{
    const char* star      = nullptr;
    const char* star_name = nullptr;
    while(*name)
    {
        if(*pat == '*')
        {
            star      = pat++;
            star_name = name;
        }
        else if(*pat == '?' || (*pat && Upper(*pat) == Upper(*name)))
        {
            pat++;
            name++;
        }
        else if(star)
        {
            pat  = star + 1;
            name = ++star_name;
        }
        else
            return false;
    }
    while(*pat == '*')
        pat++;
    return *pat == 0;
}

// ---- Volume and paths -------------------------------------------------------------------------

// Strips a drive prefix ("0:"). Only drive 0, the SD card, exists.
FRESULT StripDrive(const TCHAR*& path)
{
    if(!path)
        return FR_INVALID_NAME;
    const TCHAR* p = path;
    while(*p && *p != ':' && *p != '/' && *p != '\\')
        p++;
    if(*p != ':')
        return FR_OK;
    if(p - path != 1 || path[0] != '0')
        return FR_INVALID_DRIVE;
    path = p + 1;
    return FR_OK;
}

// What every call checks first, as FatFs's find_volume does: a mounted volume, then the card.
FRESULT Volume()
{
    if(!mounted.load(std::memory_order_acquire))
        return FR_NOT_ENABLED;
    if(!daisycola::sd::Ready())
        return FR_NOT_READY;
    return FR_OK;
}

void FillVolume(FATFS* fs)
{
    fs->fs_type  = FS_FAT32;
    fs->drv      = 0;
    fs->n_fats   = 1;
    fs->csize    = kClusterSec;
    fs->id       = mount_id;
    fs->last_clst = kMaxSize;
    fs->free_clst = kMaxSize;
    struct statvfs sv;
    if(Card() >= 0 && fstatvfs(Card(), &sv) == 0)
    {
        const uint64_t clusters = uint64_t(sv.f_blocks) * sv.f_frsize / (kClusterSec * 512u);
        fs->n_fatent = DWORD(clusters + 2 > kMaxSize ? kMaxSize : clusters + 2);
    }
}

// A FAT long name: no control characters or "*:<>?|, and no trailing dots or spaces (which FatFs
// drops).
FRESULT CheckName(char* name, size_t& len)
{
    while(len > 0 && (name[len - 1] == '.' || name[len - 1] == ' '))
        len--;
    name[len] = 0;
    if(len == 0 || len > kNameMax)
        return FR_INVALID_NAME;
    for(size_t i = 0; i < len; i++)
    {
        const unsigned char c = name[i];
        if(c < 0x20 || c == 0x7f || std::strchr("\"*:<>?|", c))
            return FR_INVALID_NAME;
    }
    return FR_OK;
}

// Finds the entry in directory `dir` (relative to the card, "" for its root) whose name matches
// `name` without regard to case, and copies its real name over `name`.
bool FindIgnoringCase(const char* dir, char* name)
{
    const int fd = openat(Card(), *dir ? dir : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(fd < 0)
        return false;
    alignas(8) char buf[2048];
    bool            found = false;
    for(ssize_t n; !found && (n = ReadEntries(fd, buf, sizeof buf)) > 0;)
        for(ssize_t off = 0; off < n;)
        {
            const auto* e = reinterpret_cast<const Dirent64*>(buf + off);
            off += e->d_reclen;
            if(SameName(e->d_name, name))
            {
                std::memcpy(name, e->d_name, std::strlen(name)); // same length: ASCII case only
                found = true;
                break;
            }
        }
    close(fd);
    return found;
}

// A path resolved on the card.
struct Path
{
    char        rel[kPathMax]; // relative to the card root, "" for the root itself
    const char* leaf;          // the last component of rel
    bool        exists;
    struct stat st; // valid if it exists

    bool        IsRoot() const { return rel[0] == 0; }
    const char* At() const { return IsRoot() ? "." : rel; }
    bool        IsDir() const { return exists && S_ISDIR(st.st_mode); }
    bool        ReadOnly() const { return (st.st_mode & 0222) == 0; }
};

// Turns a FatFs path into a path under the card folder. Each existing component is matched
// without regard to case; a last component that doesn't exist keeps the firmware's spelling.
// Fails with FR_NO_PATH if a directory on the way is missing, and FR_INVALID_NAME if ".."
// would leave the card.
FRESULT Resolve(const TCHAR* fat_path, Path& out)
{
    FRESULT res = StripDrive(fat_path);
    if(res != FR_OK)
        return res;

    // Collect the logical path, starting from the root or the current directory.
    char   logical[kPathMax];
    size_t len = 0;
    if(*fat_path != '/' && *fat_path != '\\')
    {
        len = std::strlen(cwd);
        std::memcpy(logical, cwd, len);
    }
    const TCHAR* p = fat_path;
    while(*p)
    {
        while(*p == '/' || *p == '\\')
            p++;
        if(!*p)
            break;
        char   name[kNameMax + 2];
        size_t n = 0;
        for(; *p && *p != '/' && *p != '\\'; p++)
        {
            if(n > kNameMax)
                return FR_INVALID_NAME;
            name[n++] = *p;
        }
        name[n] = 0;
        if(std::strcmp(name, ".") == 0)
            continue;
        if(std::strcmp(name, "..") == 0)
        {
            if(len == 0)
                return FR_INVALID_NAME; // above the card's root
            while(len > 0 && logical[len - 1] != '/')
                len--;
            if(len > 0)
                len--;
            continue;
        }
        res = CheckName(name, n);
        if(res != FR_OK)
            return res;
        if(len + (len ? 1 : 0) + n >= kPathMax)
            return FR_INVALID_NAME;
        if(len)
            logical[len++] = '/';
        std::memcpy(logical + len, name, n);
        len += n;
    }
    logical[len] = 0;

    // Walk it on the card, fixing the case of each component that exists.
    std::memcpy(out.rel, logical, len + 1);
    out.leaf   = out.rel;
    out.exists = true;
    if(len == 0)
        return fstat(Card(), &out.st) == 0 ? FR_OK : FR_DISK_ERR;
    for(char* comp = out.rel;;)
    {
        char* end  = std::strchr(comp, '/');
        const bool last = end == nullptr;
        if(!last)
            *end = 0;
        out.leaf = comp;
        if(fstatat(Card(), out.rel, &out.st, 0) != 0)
        {
            if(errno != ENOENT)
                return FromErrno(errno);
            char parent[kPathMax];
            const size_t plen = comp == out.rel ? 0 : size_t(comp - out.rel - 1);
            std::memcpy(parent, out.rel, plen);
            parent[plen] = 0;
            if(!FindIgnoringCase(parent, comp) || fstatat(Card(), out.rel, &out.st, 0) != 0)
            {
                if(!last)
                    return FR_NO_PATH;
                out.exists = false;
                return FR_OK;
            }
        }
        if(last)
            return FR_OK;
        if(!S_ISDIR(out.st.st_mode))
            return FR_NO_PATH;
        *end = '/';
        comp = end + 1;
    }
}

// The FAT short name FatFs reports as altname: the name itself in capitals if it fits 8.3, or
// a "NAME~1.EXT" made from it.
void ShortName(const char* name, char* alt)
{
    const char* dot     = std::strrchr(name, '.');
    const size_t base   = dot && dot != name ? size_t(dot - name) : std::strlen(name);
    const size_t ext    = dot && dot != name ? std::strlen(dot + 1) : 0;
    bool         fits   = name[0] != '.' && base >= 1 && base <= 8 && ext <= 3;
    for(const char* c = name; fits && *c; c++)
        fits = (*c > 0x20 && *c < 0x7f && !std::strchr("+,;=[] ", *c)) && (*c != '.' || c == dot);
    size_t i = 0;
    for(size_t k = 0; k < base && i < (fits ? 8u : 6u); k++)
        if(name[k] != ' ' && name[k] != '.')
            alt[i++] = Upper(name[k]);
    if(!fits)
    {
        alt[i++] = '~';
        alt[i++] = '1';
    }
    if(ext)
    {
        alt[i++] = '.';
        for(size_t k = 0; k < ext && k < 3; k++)
            alt[i++] = Upper(dot[1 + k]);
    }
    alt[i] = 0;
}

void FillInfo(FILINFO* fno, const char* name, const struct stat& st)
{
    if(!fno)
        return;
    const bool dir = S_ISDIR(st.st_mode);
    fno->fsize     = dir ? 0 : FSIZE_t(uint64_t(st.st_size) > kMaxSize ? kMaxSize : st.st_size);
    fno->fdate     = 0; // no real-time clock, as get_fattime says
    fno->ftime     = 0;
    fno->fattrib   = BYTE((dir ? AM_DIR : 0) | ((st.st_mode & 0222) == 0 ? AM_RDO : 0)
                        | (name[0] == '.' ? AM_HID : 0));
    CopyString(fno->fname, sizeof fno->fname, name);
    ShortName(name, fno->altname);
}

// ---- Open files -------------------------------------------------------------------------------

FileSlot* FindFile(const FIL* fp)
{
    if(!fp)
        return nullptr;
    for(FileSlot& s : files)
        if(s.fil.load(std::memory_order_acquire) == fp)
            return &s;
    return nullptr;
}

void ReleaseFile(FileSlot& s)
{
    const int fd = s.fd;
    s.fd         = -1;
    s.fil.store(nullptr, std::memory_order_release);
    if(fd >= 0)
        close(fd);
}

// As FatFs's validate: an open file of the mounted volume, and the card is there.
FRESULT ValidFile(FIL* fp, int& fd)
{
    FileSlot* s  = FindFile(fp);
    FATFS*    fs = mounted.load(std::memory_order_acquire);
    if(!s || !fs || fp->obj.fs != fs || fp->obj.id != fs->id)
        return FR_INVALID_OBJECT;
    if(!daisycola::sd::Ready())
        return FR_NOT_READY;
    fd = s->fd;
    return FR_OK;
}

DirSlot* FindDir(const DIR* dp)
{
    if(!dp)
        return nullptr;
    for(DirSlot& s : dirs)
        if(s.dir.load(std::memory_order_acquire) == dp)
            return &s;
    return nullptr;
}

void ReleaseDir(DirSlot& s)
{
    const int fd = s.fd;
    s.fd         = -1;
    s.dir.store(nullptr, std::memory_order_release);
    if(fd >= 0)
        close(fd);
}

FRESULT ValidDir(DIR* dp, DirSlot*& slot)
{
    slot      = FindDir(dp);
    FATFS* fs = mounted.load(std::memory_order_acquire);
    if(!slot || !fs || dp->obj.fs != fs || dp->obj.id != fs->id)
        return FR_INVALID_OBJECT;
    if(!daisycola::sd::Ready())
        return FR_NOT_READY;
    return FR_OK;
}

// The next entry of an open directory, skipping "." and ".."; fname[0] == 0 at the end.
FRESULT NextEntry(DirSlot& s, FILINFO* fno)
{
    for(;;)
    {
        if(s.pos >= s.len)
        {
            const ssize_t n = ReadEntries(s.fd, s.buf, sizeof s.buf);
            if(n < 0)
                return FromErrno(errno);
            if(n == 0)
            {
                fno->fname[0]   = 0;
                fno->altname[0] = 0;
                return FR_OK;
            }
            s.len = size_t(n);
            s.pos = 0;
        }
        const auto* e = reinterpret_cast<const Dirent64*>(s.buf + s.pos);
        s.pos += e->d_reclen;
        if(std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0
           || std::strlen(e->d_name) > kNameMax)
            continue;
        if(s.find && !Matches(s.pattern, e->d_name))
            continue;
        struct stat st;
        if(fstatat(s.fd, e->d_name, &st, 0) != 0)
            continue; // gone since, or a broken link
        FillInfo(fno, e->d_name, st);
        return FR_OK;
    }
}

// Empties a directory, recursively (f_mkfs).
bool Empty(int dir_fd, int depth)
{
    if(depth > 64)
        return false;
    alignas(8) char buf[2048];
    for(bool removed = true; removed;)
    {
        removed = false;
        lseek(dir_fd, 0, SEEK_SET);
        for(ssize_t n; (n = ReadEntries(dir_fd, buf, sizeof buf)) > 0;)
            for(ssize_t off = 0; off < n;)
            {
                const auto* e = reinterpret_cast<const Dirent64*>(buf + off);
                off += e->d_reclen;
                if(std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
                    continue;
                if(unlinkat(dir_fd, e->d_name, 0) == 0)
                {
                    removed = true;
                    continue;
                }
                if(errno != EISDIR)
                    return false;
                const int sub = openat(dir_fd, e->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                if(sub < 0)
                    return false;
                const bool ok = Empty(sub, depth + 1);
                close(sub);
                if(!ok || unlinkat(dir_fd, e->d_name, AT_REMOVEDIR) != 0)
                    return false;
                removed = true;
            }
    }
    return true;
}

// ---- String functions (FatFs's putbuff) -------------------------------------------------------

struct PutBuffer
{
    FIL* fp;
    int  idx  = 0; // -1 after an error
    int  nchr = 0;
    BYTE buf[64];

    explicit PutBuffer(FIL* f) : fp(f) {}

    void Put(TCHAR c)
    {
        if(_USE_STRFUNC == 2 && c == '\n') // LF -> CRLF
            Put('\r');
        if(idx < 0)
            return;
        buf[idx++] = BYTE(c);
        if(idx >= int(sizeof buf) - 3)
        {
            UINT bw = 0;
            f_write(fp, buf, UINT(idx), &bw);
            idx = bw == UINT(idx) ? 0 : -1;
        }
        nchr++;
    }

    int Flush()
    {
        UINT bw = 0;
        if(idx >= 0 && f_write(fp, buf, UINT(idx), &bw) == FR_OK && UINT(idx) == bw)
            return nchr;
        return EOF;
    }
};

} // namespace

namespace daisycola::sd
{
void CloseOpenFiles()
{
    for(FileSlot& s : files)
        if(s.fil.load())
            ReleaseFile(s);
    for(DirSlot& s : dirs)
        if(s.dir.load())
            ReleaseDir(s);
}
} // namespace daisycola::sd

extern "C" {

// ---- Disk status ------------------------------------------------------------------------------
//
// TAPE polls disk_status(0) to notice a pulled card. The rest of the disk layer (sector reads and
// writes) has no meaning on a folder and isn't provided.

DSTATUS disk_status(BYTE pdrv)
{
    return pdrv == 0 && daisycola::sd::Ready() ? 0 : STA_NOINIT | STA_NODISK;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return disk_status(pdrv);
}

// ---- Volume -----------------------------------------------------------------------------------

FRESULT f_mount(FATFS* fs, const TCHAR* path, BYTE opt)
{
    BusyScope    busy;
    const TCHAR* p   = path ? path : "";
    FRESULT      res = StripDrive(p);
    if(res != FR_OK)
        return res;
    // Files and directories of the volume that was mounted become invalid, as in FatFs.
    daisycola::sd::CloseOpenFiles();
    cwd[0] = 0;
    if(FATFS* old = mounted.exchange(nullptr))
        old->fs_type = 0;
    if(!fs)
        return FR_OK;
    std::memset(fs, 0, sizeof *fs);
    mount_id++;
    FillVolume(fs);
    mounted.store(fs, std::memory_order_release);
    return opt == 1 ? Volume() : FR_OK;
}

FRESULT f_getfree(const TCHAR* path, DWORD* nclst, FATFS** fatfs)
{
    BusyScope    busy;
    const TCHAR* p   = path ? path : "";
    FRESULT      res = StripDrive(p);
    if(res == FR_OK)
        res = Volume();
    if(res != FR_OK)
        return res;
    struct statvfs sv;
    if(fstatvfs(Card(), &sv) != 0)
        return FromErrno(errno);
    FATFS*         fs   = mounted.load();
    const uint64_t free = uint64_t(sv.f_bavail) * sv.f_frsize / (kClusterSec * 512u);
    FillVolume(fs);
    fs->free_clst = DWORD(free > kMaxSize ? kMaxSize : free);
    if(nclst)
        *nclst = fs->free_clst;
    if(fatfs)
        *fatfs = fs;
    return FR_OK;
}

FRESULT f_getlabel(const TCHAR* path, TCHAR* label, DWORD* vsn)
{
    BusyScope    busy;
    const TCHAR* p   = path ? path : "";
    FRESULT      res = StripDrive(p);
    if(res == FR_OK)
        res = Volume();
    if(res != FR_OK)
        return res;
    // The card's label is its folder's name, cut to a FAT label's 11 characters.
    if(label)
    {
        const char* slash = std::strrchr(daisycola::sd::FolderPath(), '/');
        const char* name  = slash ? slash + 1 : daisycola::sd::FolderPath();
        size_t      n     = std::strlen(name);
        if(n > 11)
            n = 11;
        std::memcpy(label, name, n);
        label[n] = 0;
    }
    if(vsn)
    {
        struct stat st;
        *vsn = fstat(Card(), &st) == 0 ? DWORD(st.st_ino) : 0;
    }
    return FR_OK;
}

FRESULT f_setlabel(const TCHAR* label)
{
    BusyScope busy;
    FRESULT   res = StripDrive(label);
    if(res == FR_OK)
        res = Volume();
    if(res != FR_OK)
        return res;
    // Setting the label renames the card folder. A FAT label has 1 to 11 characters, and none
    // of these.
    const size_t n = std::strlen(label);
    if(n == 0 || n > 11)
        return FR_INVALID_NAME;
    for(const TCHAR* c = label; *c; c++)
        if(BYTE(*c) < 0x20 || *c == 0x7f || std::strchr("\"*+,.:;<=>?[]|/\\", *c))
            return FR_INVALID_NAME;
    const int err = daisycola::sd::RenameFolder(label);
    return err == 0 ? FR_OK : err == EEXIST ? FR_DENIED : FromErrno(err);
}

FRESULT f_mkfs(const TCHAR* path, BYTE opt, DWORD au, void* work, UINT len)
{
    BusyScope    busy;
    const TCHAR* p   = path ? path : "";
    FRESULT      res = StripDrive(p);
    if(res != FR_OK)
        return res;
    if(!daisycola::sd::Ready())
        return FR_NOT_READY;
    // Formatting a folder card empties it. The mounted volume's open files go, as in FatFs.
    daisycola::sd::CloseOpenFiles();
    cwd[0] = 0;
    const int fd = openat(Card(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(fd < 0)
        return FromErrno(errno);
    const bool ok = Empty(fd, 0);
    close(fd);
    return ok ? FR_OK : FR_DISK_ERR;
}

FRESULT f_fdisk(BYTE pdrv, const DWORD* szt, void* work)
{
    Unsupported("f_fdisk");
    return FR_INT_ERR;
}

// ---- Files ------------------------------------------------------------------------------------

FRESULT f_open(FIL* fp, const TCHAR* path, BYTE mode)
{
    if(!fp)
        return FR_INVALID_OBJECT;
    BusyScope busy;
    // Reopening a FIL that is still open drops the old file.
    if(FileSlot* old = FindFile(fp))
        ReleaseFile(*old);
    fp->obj.fs = nullptr;

    FRESULT res = Volume();
    if(res != FR_OK)
        return res;
    mode &= FA_READ | FA_WRITE | FA_CREATE_ALWAYS | FA_CREATE_NEW | FA_OPEN_ALWAYS | FA_OPEN_APPEND;
    Path f;
    res = Resolve(path, f);
    if(res != FR_OK)
        return res;
    if(f.IsRoot())
        return FR_INVALID_NAME;

    const bool create = mode & (FA_CREATE_ALWAYS | FA_OPEN_ALWAYS | FA_CREATE_NEW);
    int        flags  = O_CLOEXEC;
    if(f.exists)
    {
        if(mode & FA_CREATE_NEW)
            return FR_EXIST;
        if(f.IsDir())
            return create ? FR_DENIED : FR_NO_FILE;
        if((mode & (FA_WRITE | FA_CREATE_ALWAYS)) && f.ReadOnly())
            return FR_DENIED;
        if(mode & FA_CREATE_ALWAYS)
            flags |= O_TRUNC;
    }
    else if(create)
        flags |= O_CREAT | O_EXCL; // with the firmware's spelling
    else
        return FR_NO_FILE;
    flags |= (mode & FA_READ) && (mode & FA_WRITE) ? O_RDWR : (mode & FA_WRITE) ? O_WRONLY : O_RDONLY;

    const int fd = openat(Card(), f.rel, flags, 0666);
    if(fd < 0)
        return FromErrno(errno);
    struct stat st;
    if(fstat(fd, &st) != 0)
    {
        close(fd);
        return FR_DISK_ERR;
    }
    FileSlot* slot = nullptr;
    for(FileSlot& s : files)
    {
        FIL* expected = nullptr;
        if(s.fil.compare_exchange_strong(expected, fp, std::memory_order_acq_rel))
        {
            slot = &s;
            break;
        }
    }
    if(!slot)
    {
        close(fd);
        return FR_TOO_MANY_OPEN_FILES;
    }
    slot->fd = fd;

    FATFS* fs       = mounted.load();
    fp->obj.fs      = fs;
    fp->obj.id      = fs->id;
    fp->obj.attr    = BYTE((st.st_mode & 0222) == 0 ? AM_RDO : 0);
    fp->obj.stat    = 0;
    fp->obj.sclust  = 0;
    fp->obj.objsize = FSIZE_t(uint64_t(st.st_size) > kMaxSize ? kMaxSize : st.st_size);
    fp->flag        = mode;
    fp->err         = 0;
    fp->fptr        = 0;
    fp->clust       = 0;
    fp->sect        = 0;
    fp->dir_sect    = 0;
    fp->dir_ptr     = nullptr;
    fp->cltbl       = nullptr;
    if((mode & FA_OPEN_APPEND) == FA_OPEN_APPEND)
        fp->fptr = fp->obj.objsize;
    return FR_OK;
}

FRESULT f_close(FIL* fp)
{
    BusyScope busy;
    FileSlot* s  = FindFile(fp);
    FATFS*    fs = mounted.load(std::memory_order_acquire);
    if(!s || !fs || fp->obj.fs != fs || fp->obj.id != fs->id)
        return FR_INVALID_OBJECT;
    // The host file is closed even if the card has gone, so nothing leaks.
    ReleaseFile(*s);
    fp->obj.fs = nullptr;
    return daisycola::sd::Ready() ? FR_OK : FR_NOT_READY;
}

FRESULT f_read(FIL* fp, void* buff, UINT btr, UINT* br)
{
    BusyScope busy;
    UINT      ignored;
    if(!br)
        br = &ignored;
    *br = 0;
    int     fd;
    FRESULT res = ValidFile(fp, fd);
    if(res != FR_OK)
        return res;
    if(fp->err)
        return FRESULT(fp->err);
    if(!(fp->flag & FA_READ))
        return FR_DENIED;
    const FSIZE_t remain = fp->fptr < fp->obj.objsize ? fp->obj.objsize - fp->fptr : 0;
    if(btr > remain)
        btr = UINT(remain);
    BYTE* out = static_cast<BYTE*>(buff);
    while(*br < btr)
    {
        const ssize_t n = pread(fd, out + *br, btr - *br, off_t(fp->fptr));
        if(n < 0 && errno == EINTR)
            continue;
        if(n < 0)
            return FR_DISK_ERR;
        if(n == 0)
            break; // shortened by someone else
        *br += UINT(n);
        fp->fptr += FSIZE_t(n);
    }
    return FR_OK;
}

// CHOMPI firmware passes bw == NULL when it doesn't need the count. On the STM32 FatFs stores it
// at address 0, ITCM RAM, unnoticed; here that would crash, so it's allowed.
FRESULT f_write(FIL* fp, const void* buff, UINT btw, UINT* bw)
{
    BusyScope busy;
    UINT      ignored;
    if(!bw)
        bw = &ignored;
    *bw = 0;
    int     fd;
    FRESULT res = ValidFile(fp, fd);
    if(res != FR_OK)
        return res;
    if(fp->err)
        return FRESULT(fp->err);
    if(!(fp->flag & FA_WRITE))
        return FR_DENIED;
    if(DWORD(fp->fptr + btw) < DWORD(fp->fptr)) // a FAT file stops short of 4 GB
        btw = UINT(kMaxSize - fp->fptr);
    const BYTE* in = static_cast<const BYTE*>(buff);
    while(*bw < btw)
    {
        const ssize_t n = pwrite(fd, in + *bw, btw - *bw, off_t(fp->fptr));
        if(n < 0 && errno == EINTR)
            continue;
        if(n < 0 && (errno == ENOSPC || errno == EDQUOT))
            break; // a full card: FatFs returns FR_OK with a short count
        if(n <= 0)
            return FR_DISK_ERR;
        *bw += UINT(n);
        fp->fptr += FSIZE_t(n);
    }
    if(fp->fptr > fp->obj.objsize)
        fp->obj.objsize = fp->fptr;
    return FR_OK;
}

FRESULT f_lseek(FIL* fp, FSIZE_t ofs)
{
    BusyScope busy;
    int       fd;
    FRESULT   res = ValidFile(fp, fd);
    if(res != FR_OK)
        return res;
    if(fp->err)
        return FRESULT(fp->err);
    // There are no clusters to map: the fast-seek table is left alone and seeks work as usual.
    if(fp->cltbl && ofs == CREATE_LINKMAP)
        return FR_OK;
    if(ofs > fp->obj.objsize && !(fp->flag & FA_WRITE))
        ofs = fp->obj.objsize; // read-only: clipped at the end
    fp->fptr = ofs;
    if(fp->fptr > fp->obj.objsize) // writable: past the end extends the file
    {
        if(ftruncate(fd, off_t(fp->fptr)) != 0)
            return FromErrno(errno);
        fp->obj.objsize = fp->fptr;
    }
    return FR_OK;
}

FRESULT f_truncate(FIL* fp)
{
    BusyScope busy;
    int       fd;
    FRESULT   res = ValidFile(fp, fd);
    if(res != FR_OK)
        return res;
    if(fp->err)
        return FRESULT(fp->err);
    if(!(fp->flag & FA_WRITE))
        return FR_DENIED;
    if(fp->fptr < fp->obj.objsize)
    {
        if(ftruncate(fd, off_t(fp->fptr)) != 0)
            return FromErrno(errno);
        fp->obj.objsize = fp->fptr;
    }
    return FR_OK;
}

// Writes go straight to the host file, and the host reads it through the same page cache.
FRESULT f_sync(FIL* fp)
{
    BusyScope busy;
    int       fd;
    return ValidFile(fp, fd);
}

FRESULT f_expand(FIL* fp, FSIZE_t fsz, BYTE opt)
{
    BusyScope busy;
    int       fd;
    FRESULT   res = ValidFile(fp, fd);
    if(res != FR_OK)
        return res;
    if(fp->err)
        return FRESULT(fp->err);
    if(fsz == 0 || fp->obj.objsize != 0 || !(fp->flag & FA_WRITE))
        return FR_DENIED;
    if(!opt) // only checks that there is room
    {
        struct statvfs sv;
        if(fstatvfs(fd, &sv) != 0)
            return FromErrno(errno);
        return uint64_t(sv.f_bavail) * sv.f_frsize >= fsz ? FR_OK : FR_DENIED;
    }
    if(fallocate(fd, 0, 0, off_t(fsz)) != 0)
    {
        if(errno != EOPNOTSUPP)
            return errno == ENOSPC ? FR_DENIED : FromErrno(errno);
        if(ftruncate(fd, off_t(fsz)) != 0)
            return FromErrno(errno);
    }
    fp->obj.objsize = fsz;
    return FR_OK;
}

FRESULT f_forward(FIL* fp, UINT (*func)(const BYTE*, UINT), UINT btf, UINT* bf)
{
    Unsupported("f_forward");
    return FR_INT_ERR;
}

// ---- Directories and names --------------------------------------------------------------------

FRESULT f_opendir(DIR* dp, const TCHAR* path)
{
    if(!dp)
        return FR_INVALID_OBJECT;
    BusyScope busy;
    if(DirSlot* old = FindDir(dp))
        ReleaseDir(*old);
    dp->obj.fs  = nullptr;
    FRESULT res = Volume();
    if(res != FR_OK)
        return res;
    Path d;
    res = Resolve(path, d);
    if(res != FR_OK)
        return res;
    if(!d.IsDir())
        return FR_NO_PATH;
    const int fd = openat(Card(), d.At(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(fd < 0)
        return FromErrno(errno) == FR_NO_FILE ? FR_NO_PATH : FromErrno(errno);
    for(DirSlot& s : dirs)
    {
        DIR* expected = nullptr;
        if(s.dir.compare_exchange_strong(expected, dp, std::memory_order_acq_rel))
        {
            s.fd   = fd;
            s.pos  = s.len = 0;
            s.find = false;
            FATFS* fs  = mounted.load();
            dp->obj.fs = fs;
            dp->obj.id = fs->id;
            dp->dptr   = 0;
            return FR_OK;
        }
    }
    close(fd);
    return FR_TOO_MANY_OPEN_FILES;
}

FRESULT f_closedir(DIR* dp)
{
    BusyScope busy;
    DirSlot*  s  = FindDir(dp);
    FATFS*    fs = mounted.load(std::memory_order_acquire);
    if(!s || !fs || dp->obj.fs != fs || dp->obj.id != fs->id)
        return FR_INVALID_OBJECT;
    ReleaseDir(*s);
    dp->obj.fs = nullptr;
    return daisycola::sd::Ready() ? FR_OK : FR_NOT_READY;
}

FRESULT f_readdir(DIR* dp, FILINFO* fno)
{
    BusyScope busy;
    DirSlot*  s;
    FRESULT   res = ValidDir(dp, s);
    if(res != FR_OK)
        return res;
    if(!fno) // f_rewinddir
    {
        lseek(s->fd, 0, SEEK_SET);
        s->pos = s->len = 0;
        return FR_OK;
    }
    return NextEntry(*s, fno);
}

FRESULT f_findnext(DIR* dp, FILINFO* fno)
{
    BusyScope busy;
    DirSlot*  s;
    FRESULT   res = ValidDir(dp, s);
    if(res != FR_OK)
        return res;
    FILINFO scratch;
    return NextEntry(*s, fno ? fno : &scratch);
}

FRESULT f_findfirst(DIR* dp, FILINFO* fno, const TCHAR* path, const TCHAR* pattern)
{
    BusyScope busy;
    FRESULT   res = f_opendir(dp, path);
    if(res != FR_OK)
        return res;
    DirSlot* s = FindDir(dp);
    s->find    = true;
    if(!CopyString(s->pattern, sizeof s->pattern, pattern ? pattern : "*"))
    {
        f_closedir(dp);
        return FR_INVALID_NAME;
    }
    return f_findnext(dp, fno);
}

FRESULT f_mkdir(const TCHAR* path)
{
    BusyScope busy;
    FRESULT   res = Volume();
    if(res != FR_OK)
        return res;
    Path d;
    res = Resolve(path, d);
    if(res != FR_OK)
        return res;
    if(d.IsRoot())
        return FR_INVALID_NAME;
    if(d.exists)
        return FR_EXIST;
    return mkdirat(Card(), d.rel, 0777) == 0 ? FR_OK : FromErrno(errno);
}

FRESULT f_unlink(const TCHAR* path)
{
    BusyScope busy;
    FRESULT   res = Volume();
    if(res != FR_OK)
        return res;
    Path f;
    res = Resolve(path, f);
    if(res != FR_OK)
        return res;
    if(f.IsRoot())
        return FR_INVALID_NAME;
    if(!f.exists)
        return FR_NO_FILE;
    if(f.ReadOnly())
        return FR_DENIED;
    if(f.IsDir())
    {
        if(std::strcmp(f.rel, cwd) == 0)
            return FR_DENIED; // FatFs won't remove the current directory
        if(unlinkat(Card(), f.rel, AT_REMOVEDIR) != 0)
            return errno == ENOTEMPTY || errno == EEXIST ? FR_DENIED : FromErrno(errno);
        return FR_OK;
    }
    return unlinkat(Card(), f.rel, 0) == 0 ? FR_OK : FromErrno(errno);
}

FRESULT f_rename(const TCHAR* path_old, const TCHAR* path_new)
{
    BusyScope busy;
    FRESULT   res = Volume();
    if(res != FR_OK)
        return res;
    Path from, to;
    res = Resolve(path_old, from);
    if(res != FR_OK)
        return res;
    if(from.IsRoot())
        return FR_INVALID_NAME;
    if(!from.exists)
        return FR_NO_FILE;
    res = Resolve(path_new, to);
    if(res != FR_OK)
        return res;
    if(to.IsRoot())
        return FR_INVALID_NAME;
    if(to.exists)
    {
        // The new name may be the old one in other capitals; anything else is in the way.
        if(to.st.st_ino != from.st.st_ino || to.st.st_dev != from.st.st_dev)
            return FR_EXIST;
        const TCHAR* p = path_new;
        StripDrive(p);
        const TCHAR* leaf = p;
        for(const TCHAR* c = p; *c; c++)
            if((*c == '/' || *c == '\\') && c[1])
                leaf = c + 1;
        char   name[kNameMax + 2];
        size_t n = 0;
        while(leaf[n] && leaf[n] != '/' && leaf[n] != '\\' && n <= kNameMax)
        {
            name[n] = leaf[n];
            n++;
        }
        name[n] = 0;
        if(CheckName(name, n) != FR_OK)
            return FR_INVALID_NAME;
        const size_t dir = size_t(to.leaf - to.rel);
        if(dir + n >= kPathMax)
            return FR_INVALID_NAME;
        std::memcpy(to.rel + dir, name, n + 1);
        return renameat(Card(), from.rel, Card(), to.rel) == 0 ? FR_OK : FromErrno(errno);
    }
    if(renameat2(Card(), from.rel, Card(), to.rel, RENAME_NOREPLACE) != 0)
        return errno == ENOENT ? FR_NO_PATH : FromErrno(errno);
    return FR_OK;
}

FRESULT f_stat(const TCHAR* path, FILINFO* fno)
{
    BusyScope busy;
    FRESULT   res = Volume();
    if(res != FR_OK)
        return res;
    Path f;
    res = Resolve(path, f);
    if(res != FR_OK)
        return res;
    if(f.IsRoot())
        return FR_INVALID_NAME;
    if(!f.exists)
        return FR_NO_FILE;
    FillInfo(fno, f.leaf, f.st);
    return FR_OK;
}

// Attributes and timestamps aren't kept: the calls succeed and change nothing.
FRESULT f_chmod(const TCHAR* path, BYTE attr, BYTE mask)
{
    BusyScope busy;
    return Volume();
}

FRESULT f_utime(const TCHAR* path, const FILINFO* fno)
{
    BusyScope busy;
    return Volume();
}

FRESULT f_chdir(const TCHAR* path)
{
    BusyScope busy;
    FRESULT   res = Volume();
    if(res != FR_OK)
        return res;
    Path d;
    res = Resolve(path, d);
    if(res != FR_OK)
        return res;
    if(!d.IsDir())
        return FR_NO_PATH;
    std::memcpy(cwd, d.rel, std::strlen(d.rel) + 1);
    return FR_OK;
}

FRESULT f_chdrive(const TCHAR* path)
{
    const TCHAR* p = path;
    return StripDrive(p);
}

FRESULT f_getcwd(TCHAR* buff, UINT len)
{
    BusyScope busy;
    FRESULT   res = Volume();
    if(res != FR_OK)
        return res;
    // With more than one volume FatFs puts the drive first: "0:/dir".
    const size_t n = std::strlen(cwd);
    if(!buff || len < n + 4)
        return FR_NOT_ENOUGH_CORE;
    std::memcpy(buff, "0:/", 3);
    std::memcpy(buff + 3, cwd, n + 1);
    return FR_OK;
}

// ---- String functions -------------------------------------------------------------------------

TCHAR* f_gets(TCHAR* buff, int len, FIL* fp)
{
    int    n = 0;
    TCHAR* p = buff;
    while(n < len - 1)
    {
        BYTE c;
        UINT rc = 0;
        f_read(fp, &c, 1, &rc);
        if(rc != 1)
            break;
        if(_USE_STRFUNC == 2 && c == '\r') // CRLF -> LF
            continue;
        *p++ = TCHAR(c);
        n++;
        if(c == '\n')
            break;
    }
    *p = 0;
    return n ? buff : nullptr;
}

int f_putc(TCHAR c, FIL* fp)
{
    PutBuffer pb(fp);
    pb.Put(c);
    return pb.Flush();
}

int f_puts(const TCHAR* str, FIL* fp)
{
    PutBuffer pb(fp);
    while(*str)
        pb.Put(*str++);
    return pb.Flush();
}

// FatFs's own formatter: %[0|-][width][l]{s,c,d,u,x,X,o,b}. Numbers are 32 bits, as on the chip.
int f_printf(FIL* fp, const TCHAR* fmt, ...)
{
    va_list   args;
    PutBuffer pb(fp);
    va_start(args, fmt);
    for(;;)
    {
        TCHAR c = *fmt++;
        if(c == 0)
            break;
        if(c != '%')
        {
            pb.Put(c);
            continue;
        }
        unsigned flags = 0, width = 0; // flags: 1 zero-pad, 2 left, 4 long, 8 negative
        c = *fmt++;
        if(c == '0')
        {
            flags = 1;
            c     = *fmt++;
        }
        else if(c == '-')
        {
            flags = 2;
            c     = *fmt++;
        }
        while(c >= '0' && c <= '9')
        {
            width = width * 10 + unsigned(c - '0');
            c     = *fmt++;
        }
        if(c == 'l' || c == 'L')
        {
            flags |= 4;
            c = *fmt++;
        }
        if(!c)
            break;
        const TCHAR type = Upper(c);
        unsigned    radix;
        switch(type)
        {
            case 'S':
            {
                const TCHAR* s = va_arg(args, const TCHAR*);
                unsigned     n = unsigned(std::strlen(s));
                if(!(flags & 2))
                    for(; n < width; n++)
                        pb.Put(' ');
                while(*s)
                    pb.Put(*s++);
                for(; n < width; n++)
                    pb.Put(' ');
                continue;
            }
            case 'C': pb.Put(TCHAR(va_arg(args, int))); continue;
            case 'B': radix = 2; break;
            case 'O': radix = 8; break;
            case 'D':
            case 'U': radix = 10; break;
            case 'X': radix = 16; break;
            default: pb.Put(c); continue;
        }
        DWORD v = (flags & 4)    ? DWORD(va_arg(args, long))
                  : type == 'D' ? DWORD(va_arg(args, int))
                                : DWORD(va_arg(args, unsigned));
        if(type == 'D' && (v & 0x80000000u))
        {
            v = 0 - v;
            flags |= 8;
        }
        char     digits[34];
        unsigned i = 0;
        do
        {
            const unsigned d = v % radix;
            v /= radix;
            digits[i++] = char(d < 10 ? '0' + d : (c == 'x' ? 'a' : 'A') + d - 10);
        } while(v && i < 32);
        if(flags & 8)
            digits[i++] = '-';
        unsigned   j   = i;
        const char pad = (flags & 1) ? '0' : ' ';
        while(!(flags & 2) && j++ < width)
            pb.Put(pad);
        do
            pb.Put(digits[--i]);
        while(i);
        while(j++ < width)
            pb.Put(pad);
    }
    va_end(args);
    return pb.Flush();
}

} // extern "C"
