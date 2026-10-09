// MIDI byte streams between the host and the firmware's UART and USB ports.
#pragma once

#include <cstddef>
#include <cstdint>

#include "daisycola/host.h"

namespace daisycola::midi
{
/** Firmware side, receive interrupt: takes bytes the host sent. */
size_t ReadIn(MidiPort port, uint8_t* data, size_t size);

/** Firmware side, main or interrupt context: queues a message for the host. A message that
 *  doesn't fit whole is dropped. */
void WriteOut(MidiPort port, const uint8_t* data, size_t size);

/** True while the host has USB plugged in. */
bool UsbConnected();

} // namespace daisycola::midi
