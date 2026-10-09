#include "per/sdmmc.h"

using namespace daisy;

// The SD card is an image file (see board/sd_card.cpp). There is no bus to set up.
SdmmcHandler::Result SdmmcHandler::Init(const Config& cfg)
{
    return Result::OK;
}
