/* daisycola replacement for libDaisy's daisy.h.
 *
 * libDaisy's daisy.h pulls in every driver, most of which need the STM32 HAL. This one includes
 * only the parts CHOMPI firmware uses. Something missing from here fails at compile time.
 * See docs/headers.md for where each header comes from.
 */
#ifndef DSY_LIBDAISY_H
#define DSY_LIBDAISY_H

#include <stdint.h>
/* Angle brackets: daisy_core.h must be found on the include path for its #include_next. */
#include <daisy_core.h>
#include "version.h"

#include "sys/system.h"
#include "per/gpio.h"
#include "per/tim.h"
#include "dev/sdram.h"
#include "dev/sr_4021.h"
#include "hid/audio.h"
#ifdef __cplusplus
#include "per/i2c.h"
#include "per/uart.h"
#include "hid/midi.h"
#include "hid/switch.h"
#include "per/sai.h"
#include "per/sdmmc.h"
#include "per/tim_channel.h"
#include "ui/UI.h"
#include "ui/UiEventQueue.h"
#include "util/scopedirqblocker.h"
#include "util/FIFO.h"
#include "util/wav_format.h"
#endif
#endif
