/* daisycola host API: how a host program drives the virtual Daisy board. */
#pragma once

namespace daisycola
{
/** The firmware's main function, renamed at compile time with -Dmain=<name>. */
using FirmwareMain = int (*)();

/** Starts the firmware on its own thread. */
void Start(FirmwareMain firmware_main);

} // namespace daisycola
