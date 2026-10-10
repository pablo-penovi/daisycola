// A firmware library's entry point (see firmware_api.h). daisycola_add_firmware compiles this file
// into every firmware library, next to the firmware renamed to daisycola_firmware_main.
#include "firmware_api.h"

#include <cstdio>
#include <exception>

#include "board/board.h"
#include "mcu/vmcu.h"

int daisycola_firmware_main();

namespace daisycola
{
namespace
{
void WriteError(char* error, size_t size, const char* what)
{
    if(error && size > 0)
        std::snprintf(error, size, "%s", what);
}

void StartLoaded()
{
    Start(daisycola_firmware_main);
}

bool Shutdown(char* error, size_t size)
{
    if(!mcu::Shutdown())
    {
        WriteError(error, size, "the firmware is still running");
        return false;
    }
    board::UnmapSdram();
    try
    {
        SdEject();
    }
    catch(const std::exception& e)
    {
        WriteError(error, size, e.what());
        return false;
    }
    return true;
}

bool Insert(const char* dir, char* error, size_t size)
{
    try
    {
        SdInsert(dir);
        return true;
    }
    catch(const std::exception& e)
    {
        WriteError(error, size, e.what());
        return false;
    }
}

bool Eject(char* error, size_t size)
{
    try
    {
        SdEject();
        return true;
    }
    catch(const std::exception& e)
    {
        WriteError(error, size, e.what());
        return false;
    }
}

const abi::FirmwareApi kApi = {
    abi::kVersion,
    sizeof(abi::FirmwareApi),

    StartLoaded,
    Halt,
    Wake,
    GetBoardState,
    GetIrqStats,
    Shutdown,

    SetPin,
    ReleasePin,
    GetPin,
    GetPinChanges,

    AttachSr4021,
    SetSrInputs,
    SetSrInput,
    GetSrInputs,
    PendingSrChanges,

    AttachEncoder,
    static_cast<void (*)(int, int)>(QueueDetents),
    static_cast<void (*)(EncoderLine, EncoderLine, int)>(QueueDetents),
    PendingDetents,

    AttachI2CDevice,

    GetDmaFrame,
    static_cast<size_t (*)(const uint32_t*, size_t, uint32_t, uint32_t, ColorOrder, Rgb*, size_t, double)>(
        Ws2812Decode),

    GetAudioFormat,
    SetAudioClock,
    ProcessAudio,
    WriteAudio,
    ReadAudio,
    GetAudioStats,

    WriteMidiIn,
    ReadMidiOut,
    SetUsbConnected,

    UseManualClock,
    AdvanceClock,
    ServiceInterrupts,

    Insert,
    Eject,
    SdSetPresent,
    SdBusy,
};
} // namespace
} // namespace daisycola

extern "C" __attribute__((visibility("default"))) const daisycola::abi::FirmwareApi*
daisycola_firmware_api()
{
    return &daisycola::kApi;
}
