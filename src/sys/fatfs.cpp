#include "sys/fatfs.h"

#include <cstdio>
#include <cstdlib>

#include "board/sd_card.h"

using namespace daisy;

FatFSInterface::Result FatFSInterface::Init(const FatFSInterface::Config& cfg)
{
    Result ret = Result::ERR_NO_MEDIA_SELECTED;
    cfg_       = cfg;
    if(cfg_.media & Config::MEDIA_SD)
        ret = FATFS_LinkDriver(&daisycola::sd::kDriver, path_[0]) == FR_OK
                  ? Result::OK
                  : Result::ERR_TOO_MANY_VOLUMES;
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
