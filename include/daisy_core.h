/* daisycola: libDaisy's daisy_core.h with the memory-section macros emptied.
 *
 * On the Seed these macros place buffers in DMA-safe or tightly-coupled RAM. On the host every
 * buffer lives in ordinary memory.
 */
#pragma once
#include_next "daisy_core.h"

#undef DMA_BUFFER_MEM_SECTION
#define DMA_BUFFER_MEM_SECTION
#undef DTCM_MEM_SECTION
#define DTCM_MEM_SECTION
