// daisycola_host: host.h for a firmware library.
//
// Every call goes through the loaded library's table (firmware_api.h). A call holds the library
// loaded until it returns: it counts itself in `in_flight` and only then reads the table, and an
// unload clears the table first and then waits for the count to drop. The counter and the table
// are lock-free atomics, so the host's audio thread can call in while another thread power-cycles.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <thread>

#include "daisycola/host.h"
#include "firmware_api.h"

namespace daisycola
{
namespace
{
using Api = abi::FirmwareApi;

std::atomic<const Api*> api{nullptr};
std::atomic<int>        in_flight{0};

// Load, unload and power cycles take turns.
std::mutex  lifecycle;
std::string library_path;
void*       library = nullptr;
bool        stuck   = false; // an unload failed halfway: the old library may still be mapped

[[noreturn]] void Fail(const std::string& message)
{
    std::fprintf(stderr, "daisycola: %s\n", message.c_str());
    std::abort();
}

// One call into the library. Empty while no library is loaded.
class Call
{
  public:
    Call()
    {
        in_flight.fetch_add(1);
        api_ = api.load();
        if(!api_)
            in_flight.fetch_sub(1);
    }
    ~Call()
    {
        if(api_)
            in_flight.fetch_sub(1);
    }
    Call(const Call&)            = delete;
    Call& operator=(const Call&) = delete;

    explicit operator bool() const { return api_ != nullptr; }
    const Api* operator->() const { return api_; }

  private:
    const Api* api_;
};

// A call that makes no sense without a library: wiring, inserting the card, starting.
const Api& Need(const Call& call, const char* what)
{
    if(!call)
        Fail(std::string(what) + " needs a firmware library: call LoadFirmware first");
    return *call.operator->();
}

void LoadLocked(const std::string& path)
{
    // dlopen would hand back the old library, state and all.
    if(stuck)
        throw FirmwareError("the last firmware library couldn't be unloaded, so it can't be "
                            "loaded again; restart the program");
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if(!handle)
        throw FirmwareError("can't load firmware library " + path + ": " + dlerror());
    using Entry = const Api* (*)();
    const auto  entry = reinterpret_cast<Entry>(dlsym(handle, "daisycola_firmware_api"));
    const Api*  table = entry ? entry() : nullptr;
    std::string problem;
    if(!table)
        problem = " has no daisycola_firmware_api(): build it with daisycola_add_firmware";
    else if(table->version != abi::kVersion || table->size != sizeof(Api))
        problem = " comes from a different daisycola build";
    if(!problem.empty())
    {
        dlclose(handle);
        throw FirmwareError("firmware library " + path + problem);
    }
    library      = handle;
    library_path = path;
    api.store(table);
}

void UnloadLocked(uint32_t halt_timeout_ms)
{
    const Api* table = api.load();
    if(!table)
        return;

    // Halt first, with the library still in use by the host: if the firmware doesn't stop,
    // nothing has changed.
    if(!table->halt(halt_timeout_ms))
        throw FirmwareError("the firmware didn't halt within " + std::to_string(halt_timeout_ms)
                            + " ms; it is still running");

    // From here on host calls find no library. Wait for those already inside it.
    api.store(nullptr);
    while(in_flight.load() > 0)
        std::this_thread::sleep_for(std::chrono::microseconds(50));

    stuck           = true;
    char error[256] = "";
    if(!table->shutdown(error, sizeof error))
        throw FirmwareError(std::string("can't release the firmware library: ") + error);

    void* handle = library;
    library      = nullptr;
    if(dlclose(handle) != 0)
        throw FirmwareError("can't unload firmware library " + library_path + ": " + dlerror());

    // dlclose doesn't promise to unmap. If anything kept the library loaded (a GNU unique
    // symbol, a thread_local with a destructor, another dlopen), its globals would carry over.
    if(void* still = dlopen(library_path.c_str(), RTLD_NOW | RTLD_NOLOAD))
    {
        dlclose(still);
        throw FirmwareError("firmware library " + library_path
                            + " is still loaded after dlclose, so its state would carry over; "
                              "build it with daisycola_add_firmware");
    }
    stuck = false;
}

void Silence(float* const* out, size_t frames)
{
    if(!out)
        return;
    for(size_t c = 0; c < kMaxAudioChannels; c++)
        if(out[c])
            std::memset(out[c], 0, frames * sizeof(float));
}
} // namespace

// ---- Library lifecycle --------------------------------------------------------------------------

void LoadFirmware(const std::string& path)
{
    std::lock_guard<std::mutex> lock(lifecycle);
    if(api.load())
        throw FirmwareError("a firmware library is already loaded; power-cycle it instead");
    LoadLocked(path);
}

void PowerCycle(uint32_t halt_timeout_ms)
{
    std::lock_guard<std::mutex> lock(lifecycle);
    if(!api.load())
        throw FirmwareError("no firmware library is loaded");
    const std::string path = library_path;
    UnloadLocked(halt_timeout_ms);
    LoadLocked(path);
}

void UnloadFirmware(uint32_t halt_timeout_ms)
{
    std::lock_guard<std::mutex> lock(lifecycle);
    UnloadLocked(halt_timeout_ms);
}

// ---- Firmware -----------------------------------------------------------------------------------

void Start()
{
    Call c;
    Need(c, "Start").start();
}

bool Halt(uint32_t timeout_ms)
{
    Call c;
    return c ? c->halt(timeout_ms) : true;
}

void Wake()
{
    if(Call c; c)
        c->wake();
}

BoardState GetBoardState()
{
    Call c;
    return c ? c->get_board_state() : BoardState{};
}

IrqStats GetIrqStats(Irq irq)
{
    Call c;
    return c ? c->get_irq_stats(irq) : IrqStats{};
}

// ---- Pins ---------------------------------------------------------------------------------------

void SetPin(daisy::Pin pin, bool level)
{
    if(Call c; c)
        c->set_pin(pin, level);
}

void ReleasePin(daisy::Pin pin)
{
    if(Call c; c)
        c->release_pin(pin);
}

bool GetPin(daisy::Pin pin)
{
    Call c;
    return c && c->get_pin(pin);
}

uint32_t GetPinChanges(daisy::Pin pin)
{
    Call c;
    return c ? c->get_pin_changes(pin) : 0;
}

// ---- CD4021 shift registers and encoders --------------------------------------------------------

int AttachSr4021(daisy::Pin clk, daisy::Pin latch, daisy::Pin data, int chips, int hold_reads)
{
    Call c;
    return Need(c, "AttachSr4021").attach_sr4021(clk, latch, data, chips, hold_reads);
}

void SetSrInputs(int chain, uint64_t levels)
{
    if(Call c; c)
        c->set_sr_inputs(chain, levels);
}

void SetSrInput(int chain, int bit, bool level)
{
    if(Call c; c)
        c->set_sr_input(chain, bit, level);
}

uint64_t GetSrInputs(int chain)
{
    Call c;
    return c ? c->get_sr_inputs(chain) : 0;
}

int PendingSrChanges(int chain)
{
    Call c;
    return c ? c->pending_sr_changes(chain) : 0;
}

int AttachEncoder(EncoderLine a, EncoderLine b, uint32_t dwell_us)
{
    Call c;
    return Need(c, "AttachEncoder").attach_encoder(a, b, dwell_us);
}

void QueueDetents(int encoder, int detents)
{
    if(Call c; c)
        c->queue_detents(encoder, detents);
}

void QueueDetents(EncoderLine a, EncoderLine b, int detents)
{
    if(Call c; c)
        c->queue_detents_on(a, b, detents);
}

int PendingDetents(int encoder)
{
    Call c;
    return c ? c->pending_detents(encoder) : 0;
}

// ---- I2C and timer DMA --------------------------------------------------------------------------

void AttachI2CDevice(int bus, uint8_t address, I2CDevice* device)
{
    Call c;
    Need(c, "AttachI2CDevice").attach_i2c_device(bus, address, device);
}

bool GetDmaFrame(int timer, int channel, DmaFrame& frame)
{
    Call c;
    return c && c->get_dma_frame(timer, channel, frame);
}

size_t Ws2812Decode(const uint32_t* duty,
                    size_t          count,
                    uint32_t        period,
                    uint32_t        prescaler,
                    ColorOrder      order,
                    Rgb*            leds,
                    size_t          max_leds,
                    double          reset_us)
{
    Call c;
    return c ? c->ws2812_decode(duty, count, period, prescaler, order, leds, max_leds, reset_us)
             : 0;
}

size_t Ws2812Decode(const DmaFrame& frame, ColorOrder order, Rgb* leds, size_t max_leds)
{
    return Ws2812Decode(
        frame.duty, frame.count, frame.period, frame.prescaler, order, leds, max_leds);
}

// ---- Audio --------------------------------------------------------------------------------------

AudioFormat GetAudioFormat()
{
    Call c;
    return c ? c->get_audio_format() : AudioFormat{};
}

void SetAudioClock(AudioClock clock)
{
    Call c;
    Need(c, "SetAudioClock").set_audio_clock(clock);
}

bool ProcessAudio(const float* const* in, float* const* out, size_t frames)
{
    Call c;
    if(c)
        return c->process_audio(in, out, frames);
    Silence(out, frames);
    return false;
}

size_t WriteAudio(const float* const* in, size_t frames)
{
    Call c;
    return c ? c->write_audio(in, frames) : 0;
}

size_t ReadAudio(float* const* out, size_t frames)
{
    Call c;
    return c ? c->read_audio(out, frames) : 0;
}

AudioStats GetAudioStats()
{
    Call c;
    return c ? c->get_audio_stats() : AudioStats{};
}

// ---- MIDI ---------------------------------------------------------------------------------------

size_t WriteMidiIn(MidiPort port, const uint8_t* data, size_t size)
{
    Call c;
    return c ? c->write_midi_in(port, data, size) : 0;
}

size_t ReadMidiOut(MidiPort port, uint8_t* data, size_t size)
{
    Call c;
    return c ? c->read_midi_out(port, data, size) : 0;
}

void SetUsbConnected(bool connected)
{
    Call c;
    Need(c, "SetUsbConnected").set_usb_connected(connected);
}

// ---- Single-threaded testing --------------------------------------------------------------------

void UseManualClock(bool manual)
{
    Call c;
    Need(c, "UseManualClock").use_manual_clock(manual);
}

void AdvanceClock(uint64_t microseconds)
{
    Call c;
    Need(c, "AdvanceClock").advance_clock(microseconds);
}

size_t ServiceInterrupts()
{
    Call c;
    return Need(c, "ServiceInterrupts").service_interrupts();
}

// ---- SD card ------------------------------------------------------------------------------------

void SdInsert(const std::string& dir)
{
    Call c;
    char error[512] = "";
    if(!Need(c, "SdInsert").sd_insert(dir.c_str(), error, sizeof error))
        throw SdError(error);
}

void SdEject()
{
    Call c;
    char error[512] = "";
    if(c && !c->sd_eject(error, sizeof error))
        throw SdError(error);
}

void SdSetPresent(bool present)
{
    if(Call c; c)
        c->sd_set_present(present);
}

bool SdBusy()
{
    Call c;
    return c && c->sd_busy();
}

} // namespace daisycola
