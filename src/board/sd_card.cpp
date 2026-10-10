#include "board/sd_card.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "daisycola/host.h"
#include "mcu/vmcu.h"

namespace daisycola
{
namespace
{
std::atomic<int>  folder_fd{-1};
std::atomic<bool> card_present{true};
std::atomic<int>  card_busy{0};
char              folder_path[PATH_MAX] = "";

void CheckNotRunning(const char* what)
{
    if(mcu::FirmwareRunning())
        throw SdError(std::string("SD card: ") + what + " while the firmware is running");
}
} // namespace

int sd::FolderFd()
{
    return folder_fd.load(std::memory_order_acquire);
}

bool sd::Ready()
{
    const int fd = FolderFd();
    if(fd < 0 || !card_present.load(std::memory_order_acquire))
        return false;
    // A folder that was deleted stays open, with no links left. The card is gone then, as if it
    // had been pulled; it's never recreated behind the host's back.
    struct stat st;
    return fstat(fd, &st) == 0 && st.st_nlink > 0;
}

const char* sd::FolderPath()
{
    return folder_path;
}

int sd::RenameFolder(const char* name)
{
    char        to[PATH_MAX];
    const char* slash = std::strrchr(folder_path, '/');
    const int   dir   = slash ? int(slash - folder_path) + 1 : 0;
    if(std::snprintf(to, sizeof to, "%.*s%s", dir, folder_path, name) >= int(sizeof to))
        return ENAMETOOLONG;
    if(std::strcmp(to, folder_path) == 0)
        return 0;
    if(renameat2(AT_FDCWD, folder_path, AT_FDCWD, to, RENAME_NOREPLACE) != 0)
        return errno;
    std::memcpy(folder_path, to, sizeof to);
    return 0;
}

sd::BusyScope::BusyScope()
{
    card_busy.fetch_add(1, std::memory_order_acq_rel);
}

sd::BusyScope::~BusyScope()
{
    card_busy.fetch_sub(1, std::memory_order_acq_rel);
}

void SdInsert(const std::string& dir)
{
    CheckNotRunning("SdInsert");
    if(dir.empty())
        throw SdError("SD card: no folder given");
    // Relative paths are kept absolute, so a later chdir in the host doesn't move the card.
    char path[PATH_MAX];
    if(!realpath(dir.c_str(), path))
        throw SdError("SD card: cannot use " + dir + ": " + std::strerror(errno));
    const int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(fd < 0)
        throw SdError("SD card: cannot open " + dir + ": " + std::strerror(errno));
    sd::CloseOpenFiles();
    std::memcpy(folder_path, path, sizeof path);
    const int old = folder_fd.exchange(fd, std::memory_order_acq_rel);
    if(old >= 0)
        close(old);
}

void SdEject()
{
    CheckNotRunning("SdEject");
    sd::CloseOpenFiles();
    const int old = folder_fd.exchange(-1, std::memory_order_acq_rel);
    if(old >= 0)
        close(old);
    folder_path[0] = 0;
}

void SdSetPresent(bool present)
{
    card_present.store(present, std::memory_order_release);
}

bool SdBusy()
{
    return card_busy.load(std::memory_order_acquire) > 0;
}

} // namespace daisycola
