#include "sys/fatfs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace daisy;

// The SD card is a host folder, and FatFs's API acts on it directly (sys/ff_folder.cpp): there
// is no disk driver to link. Drive 0 is the card, as on the device.
FatFSInterface::Result FatFSInterface::Init(const FatFSInterface::Config& cfg)
{
    Result ret = Result::ERR_NO_MEDIA_SELECTED;
    cfg_       = cfg;
    if(cfg_.media & Config::MEDIA_SD)
    {
        std::strcpy(path_[0], "0:/");
        ret = Result::OK;
    }
    if(cfg_.media & Config::MEDIA_USB)
    {
        std::fprintf(stderr, "daisycola: USB mass storage is not supported\n");
        std::abort();
    }
    if(ret == Result::OK)
        initialized_ = true;
    return ret;
}

FatFSInterface::Result FatFSInterface::Init(const uint8_t media)
{
    cfg_.media = media;
    return Init(cfg_);
}

// libDaisy has no real-time clock: files get FatFs's zero timestamp, as on the device.
extern "C" DWORD get_fattime(void)
{
    return 0;
}
