/* daisycola: firmware includes "fatfs.h" (libDaisy's sys/fatfs.h). This makes sure FatFs sees
 * 32-bit integer types first (see daisycola/ff_integer.h). */
#pragma once
#include "daisycola/ff_integer.h"
#include "sys/fatfs.h"
