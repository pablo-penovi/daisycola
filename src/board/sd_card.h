// The virtual SD card: a FatFs disk driver backed by an image file.
#pragma once

#include "ff.h"
#include "ff_gen_drv.h"

namespace daisycola::sd
{
/** FatFs driver for the card image. The firmware reaches it through FatFSInterface. */
extern const Diskio_drvTypeDef kDriver;

} // namespace daisycola::sd
