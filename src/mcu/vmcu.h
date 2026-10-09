// The virtual MCU: firmware thread, interrupts, clock.
#pragma once

namespace daisycola::mcu
{
/** True once the firmware thread has started and until it halts. */
bool FirmwareRunning();

} // namespace daisycola::mcu
