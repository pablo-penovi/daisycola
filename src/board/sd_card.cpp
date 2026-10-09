#include "board/sd_card.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

#include "daisycola/host.h"
#include "mcu/vmcu.h"

namespace fs = std::filesystem;

namespace daisycola
{
namespace
{
constexpr UINT kSectorSize = 512;

// SD cards report a 4 MB allocation unit. f_mkfs aligns the data area to it.
constexpr DWORD kEraseBlockSectors = 8192;

std::atomic<int>      image_fd{-1};
std::atomic<uint64_t> image_sectors{0};
std::atomic<bool>     card_present{true};
std::atomic<int>      card_busy{0};

// Counts nested accesses: a timer interrupt can reach the card while the main loop is using it.
class BusyScope
{
  public:
    BusyScope() { card_busy.fetch_add(1, std::memory_order_acq_rel); }
    ~BusyScope() { card_busy.fetch_sub(1, std::memory_order_acq_rel); }
};

DSTATUS CardStatus(BYTE)
{
    if(image_fd.load(std::memory_order_acquire) < 0
       || !card_present.load(std::memory_order_acquire))
        return STA_NOINIT | STA_NODISK;
    return 0;
}

DSTATUS CardInitialize(BYTE lun)
{
    return CardStatus(lun);
}

DRESULT CardRead(BYTE lun, BYTE* buff, DWORD sector, UINT count)
{
    BusyScope busy;
    if(CardStatus(lun) != 0)
        return RES_NOTRDY;
    const int fd     = image_fd.load(std::memory_order_acquire);
    size_t    remain = size_t(count) * kSectorSize;
    off_t     offset = off_t(sector) * kSectorSize;
    while(remain > 0)
    {
        const ssize_t n = pread(fd, buff, remain, offset);
        if(n < 0 && errno == EINTR)
            continue;
        if(n < 0)
            return RES_ERROR;
        if(n == 0) // past the end of a short image: reads as erased
        {
            std::memset(buff, 0, remain);
            break;
        }
        buff += n;
        offset += n;
        remain -= size_t(n);
    }
    return RES_OK;
}

DRESULT CardWrite(BYTE lun, const BYTE* buff, DWORD sector, UINT count)
{
    BusyScope busy;
    if(CardStatus(lun) != 0)
        return RES_NOTRDY;
    const int fd     = image_fd.load(std::memory_order_acquire);
    size_t    remain = size_t(count) * kSectorSize;
    off_t     offset = off_t(sector) * kSectorSize;
    while(remain > 0)
    {
        const ssize_t n = pwrite(fd, buff, remain, offset);
        if(n < 0 && errno == EINTR)
            continue;
        if(n <= 0)
            return RES_ERROR;
        buff += n;
        offset += n;
        remain -= size_t(n);
    }
    return RES_OK;
}

DRESULT CardIoctl(BYTE lun, BYTE cmd, void* buff)
{
    if(CardStatus(lun) != 0)
        return RES_NOTRDY;
    switch(cmd)
    {
        // Writes go straight to the file. Leaving the data in the page cache is fine: the host
        // reads the image through the same cache.
        case CTRL_SYNC: return RES_OK;
        case GET_SECTOR_COUNT:
            *static_cast<DWORD*>(buff) = DWORD(image_sectors.load());
            return RES_OK;
        case GET_SECTOR_SIZE: *static_cast<WORD*>(buff) = kSectorSize; return RES_OK;
        case GET_BLOCK_SIZE: *static_cast<DWORD*>(buff) = kEraseBlockSectors; return RES_OK;
        default: return RES_PARERR;
    }
}

const char* ResultName(FRESULT res)
{
    static const char* const names[] = {
        "FR_OK",           "FR_DISK_ERR",          "FR_INT_ERR",        "FR_NOT_READY",
        "FR_NO_FILE",      "FR_NO_PATH",           "FR_INVALID_NAME",   "FR_DENIED",
        "FR_EXIST",        "FR_INVALID_OBJECT",    "FR_WRITE_PROTECTED", "FR_INVALID_DRIVE",
        "FR_NOT_ENABLED",  "FR_NO_FILESYSTEM",     "FR_MKFS_ABORTED",   "FR_TIMEOUT",
        "FR_LOCKED",       "FR_NOT_ENOUGH_CORE",   "FR_TOO_MANY_OPEN_FILES", "FR_INVALID_PARAMETER",
    };
    return unsigned(res) < sizeof(names) / sizeof(names[0]) ? names[res] : "FR_?";
}

void Check(FRESULT res, const std::string& what)
{
    if(res != FR_OK)
        throw SdError("SD card: " + what + ": " + ResultName(res));
}

void CheckNotRunning(const char* what)
{
    if(mcu::FirmwareRunning())
        throw SdError(std::string("SD card: ") + what + " while the firmware is running");
}

int OpenImageFile(const std::string& path, int flags)
{
    const int fd = open(path.c_str(), flags | O_CLOEXEC, 0644);
    if(fd < 0)
        throw SdError("SD card: cannot open " + path + ": " + std::strerror(errno));
    return fd;
}

uint64_t SectorCount(int fd, const std::string& path)
{
    struct stat st;
    if(fstat(fd, &st) != 0)
        throw SdError("SD card: cannot stat " + path + ": " + std::strerror(errno));
    return uint64_t(st.st_size) / kSectorSize;
}

// Links the card driver to a FatFs drive for the duration of a helper call.
class Drive
{
  public:
    Drive()
    {
        if(FATFS_LinkDriver(&sd::kDriver, path_) != 0)
            throw SdError("SD card: no free FatFs drive");
    }
    ~Drive() { FATFS_UnLinkDriver(path_); }

    /** Turns a card path such as "/dir/file" into a FatFs path such as "0:/dir/file". */
    std::string Path(const std::string& card_path) const
    {
        std::string p = path_; // "N:/"
        p.pop_back();
        if(card_path.empty() || card_path[0] != '/')
            p += '/';
        return p + card_path;
    }

    const char* Root() const { return path_; }

  private:
    char path_[4] = {};
};

// Mounts the inserted card for the duration of a helper call.
class Volume : public Drive
{
  public:
    Volume()
    {
        if(image_fd.load() < 0)
            throw SdError("SD card: no image is open");
        fs_ = std::make_unique<FATFS>();
        Check(f_mount(fs_.get(), Root(), 1), "mount");
    }
    ~Volume() { f_mount(nullptr, Root(), 0); }

  private:
    std::unique_ptr<FATFS> fs_;
};

void MakeCardDirs(const Volume& vol, const std::string& card_dir)
{
    std::string partial;
    for(const auto& part : fs::path(card_dir))
    {
        if(part == "/" || part.empty())
            continue;
        partial += "/" + part.string();
        const FRESULT res = f_mkdir(vol.Path(partial).c_str());
        if(res != FR_OK && res != FR_EXIST)
            Check(res, "mkdir " + partial);
    }
}

void CopyFileIn(const Volume& vol, const fs::path& host_file, const std::string& card_file)
{
    const int fd = OpenImageFile(host_file.string(), O_RDONLY);
    FIL       fil;
    FRESULT   res = f_open(&fil, vol.Path(card_file).c_str(), FA_CREATE_ALWAYS | FA_WRITE);
    if(res != FR_OK)
    {
        close(fd);
        Check(res, "create " + card_file);
    }
    std::vector<uint8_t> buf(256 * 1024);
    for(;;)
    {
        const ssize_t n = read(fd, buf.data(), buf.size());
        if(n < 0 && errno == EINTR)
            continue;
        if(n <= 0)
        {
            res = n < 0 ? FR_DISK_ERR : FR_OK;
            break;
        }
        UINT written = 0;
        res          = f_write(&fil, buf.data(), UINT(n), &written);
        if(res == FR_OK && written != UINT(n))
            res = FR_DENIED; // card full
        if(res != FR_OK)
            break;
    }
    close(fd);
    const FRESULT close_res = f_close(&fil);
    Check(res, "write " + card_file);
    Check(close_res, "close " + card_file);
}

void CopyDirIn(const Volume& vol, const fs::path& host_dir, const std::string& card_dir)
{
    MakeCardDirs(vol, card_dir);
    for(const auto& entry : fs::directory_iterator(host_dir))
    {
        const std::string name      = entry.path().filename().string();
        const std::string card_path = (card_dir == "/" ? "" : card_dir) + "/" + name;
        if(entry.is_directory())
            CopyDirIn(vol, entry.path(), card_path);
        else if(entry.is_regular_file())
            CopyFileIn(vol, entry.path(), card_path);
    }
}

std::vector<uint8_t> ReadCardFile(const Volume& vol, const std::string& card_file)
{
    FIL fil;
    Check(f_open(&fil, vol.Path(card_file).c_str(), FA_READ), "open " + card_file);
    std::vector<uint8_t> data(f_size(&fil));
    UINT                 got = 0;
    const FRESULT        res = f_read(&fil, data.data(), UINT(data.size()), &got);
    f_close(&fil);
    Check(res, "read " + card_file);
    data.resize(got);
    return data;
}

std::vector<SdEntry> ListCardDir(const Volume& vol, const std::string& card_dir)
{
    DIR dir;
    Check(f_opendir(&dir, vol.Path(card_dir).c_str()), "opendir " + card_dir);
    std::vector<SdEntry> entries;
    FILINFO              info;
    FRESULT              res;
    while((res = f_readdir(&dir, &info)) == FR_OK && info.fname[0] != 0)
    {
        const std::string name = info.fname;
        if(name == "." || name == "..")
            continue;
        entries.push_back({name, info.fsize, (info.fattrib & AM_DIR) != 0});
    }
    f_closedir(&dir);
    Check(res, "readdir " + card_dir);
    return entries;
}

void WriteHostFile(const fs::path& host_file, const std::vector<uint8_t>& data)
{
    const int fd = OpenImageFile(host_file.string(), O_WRONLY | O_CREAT | O_TRUNC);
    size_t    done = 0;
    while(done < data.size())
    {
        const ssize_t n = write(fd, data.data() + done, data.size() - done);
        if(n < 0 && errno == EINTR)
            continue;
        if(n <= 0)
        {
            close(fd);
            throw SdError("SD card: cannot write " + host_file.string() + ": "
                          + std::strerror(errno));
        }
        done += size_t(n);
    }
    close(fd);
}

void CopyOut(const Volume& vol, const std::string& card_path, const fs::path& host_dir, bool is_dir)
{
    if(!is_dir)
    {
        WriteHostFile(host_dir / fs::path(card_path).filename(), ReadCardFile(vol, card_path));
        return;
    }
    fs::create_directories(host_dir);
    for(const auto& entry : ListCardDir(vol, card_path))
    {
        const std::string child = (card_path == "/" ? "" : card_path) + "/" + entry.name;
        if(entry.is_dir)
            CopyOut(vol, child, host_dir / entry.name, true);
        else
            CopyOut(vol, child, host_dir, false);
    }
}

} // namespace

const Diskio_drvTypeDef sd::kDriver
    = {CardInitialize, CardStatus, CardRead, CardWrite, CardIoctl};

void SdCreateImage(const std::string& path, uint64_t size_bytes)
{
    CheckNotRunning("SdCreateImage");
    const int fd = OpenImageFile(path, O_RDWR | O_CREAT | O_TRUNC);
    if(ftruncate(fd, off_t(size_bytes)) != 0)
    {
        close(fd);
        throw SdError("SD card: cannot size " + path + ": " + std::strerror(errno));
    }

    // Point the driver at the new file while formatting, then put the old card back.
    const int      old_fd      = image_fd.exchange(fd);
    const uint64_t old_sectors = image_sectors.exchange(size_bytes / kSectorSize);
    FRESULT        res;
    {
        Drive                drive;
        std::vector<uint8_t> work(64 * 1024);
        res = f_mkfs(drive.Root(), FM_FAT32, 0, work.data(), UINT(work.size()));
    }
    image_fd.store(old_fd);
    image_sectors.store(old_sectors);
    close(fd);
    Check(res, "format " + path);
}

void SdOpenImage(const std::string& path)
{
    CheckNotRunning("SdOpenImage");
    const int fd = OpenImageFile(path, O_RDWR);
    image_sectors.store(SectorCount(fd, path));
    const int old = image_fd.exchange(fd);
    if(old >= 0)
        close(old);
}

void SdCloseImage()
{
    CheckNotRunning("SdCloseImage");
    const int old = image_fd.exchange(-1);
    if(old >= 0)
        close(old);
}

void SdSetPresent(bool present)
{
    card_present.store(present);
}

bool SdBusy()
{
    return card_busy.load(std::memory_order_acquire) > 0;
}

std::vector<SdEntry> SdList(const std::string& card_dir)
{
    CheckNotRunning("SdList");
    Volume vol;
    return ListCardDir(vol, card_dir);
}

void SdCopyIn(const std::string& host_path, const std::string& card_dir)
{
    CheckNotRunning("SdCopyIn");
    Volume         vol;
    const fs::path src(host_path);
    if(fs::is_directory(src))
        CopyDirIn(vol, src, card_dir);
    else
    {
        MakeCardDirs(vol, card_dir);
        CopyFileIn(vol,
                   src,
                   (card_dir == "/" ? "" : card_dir) + "/" + src.filename().string());
    }
}

void SdCopyOut(const std::string& card_path, const std::string& host_dir)
{
    CheckNotRunning("SdCopyOut");
    Volume  vol;
    FILINFO info;
    bool    is_dir = card_path == "/" || card_path.empty();
    if(!is_dir)
    {
        Check(f_stat(vol.Path(card_path).c_str(), &info), "stat " + card_path);
        is_dir = (info.fattrib & AM_DIR) != 0;
    }
    fs::create_directories(host_dir);
    CopyOut(vol, is_dir && card_path.empty() ? "/" : card_path, host_dir, is_dir);
}

std::vector<uint8_t> SdReadFile(const std::string& card_path)
{
    CheckNotRunning("SdReadFile");
    Volume vol;
    return ReadCardFile(vol, card_path);
}

} // namespace daisycola
