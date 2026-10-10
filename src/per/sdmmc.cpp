#include "per/sdmmc.h"

using namespace daisy;

// The SD card is a host folder (see board/sd_card.cpp). There is no bus to set up.
SdmmcHandler::Result SdmmcHandler::Init(const Config& cfg)
{
    return Result::OK;
}
