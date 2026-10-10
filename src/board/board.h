// The virtual board: the pin table and the chips wired to pins.
//
// The firmware side (GPIO reads and writes) runs on the MCU thread. The host sets input levels
// through atomics. Chips are wired up by the host before the firmware starts.
#pragma once

#include <cstdint>

#include "daisy_core.h"

namespace daisycola::board
{
enum class PinMode : uint8_t
{
    kInput,
    kOutput,
};

enum class PinPull : uint8_t
{
    kNone,
    kUp,
    kDown,
};

/** Firmware side: configures a pin. Invalid pins (PORTX) are ignored. */
void ConfigurePin(daisy::Pin pin, PinMode mode, PinPull pull);

/** Firmware side: the level the firmware reads on a pin. Invalid pins read 0. */
bool ReadPin(daisy::Pin pin);

/** Firmware side: drives an output pin. Chips wired to the pin see the edge. */
void WritePin(daisy::Pin pin, bool level);

/** Maps the Seed's 64 MB of SDRAM at 0xC0000000, for firmware that uses raw SDRAM addresses.
 *  Warns and carries on if the address range is taken. */
void MapSdram();

/** Unmaps the SDRAM, before a firmware library is unloaded. */
void UnmapSdram();

} // namespace daisycola::board
