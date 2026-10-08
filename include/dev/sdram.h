/* daisycola replacement for libDaisy's dev/sdram.h.
 *
 * Same declarations, but DSY_SDRAM_BSS and DSY_SDRAM_DATA are empty: buffers that live in the
 * Seed's SDRAM are ordinary globals on the host. Firmware that uses raw SDRAM addresses still
 * works, because daisycola maps 64 MB at 0xC0000000 when the board starts.
 */
#pragma once
#ifndef RAM_AS4C16M16SA_H
#define RAM_AS4C16M16SA_H
#include <stdint.h>
#include "daisy_core.h"

#define DSY_SDRAM_DATA
#define DSY_SDRAM_BSS

class SdramHandle
{
  public:
    enum class Result
    {
        OK,
        ERR,
    };

    Result Init();
    Result DeInit();

  private:
    Result PeriphInit();
    Result DeviceInit();
    Result PeriphDeInit();
    Result DeviceDeInit();
};

#endif
