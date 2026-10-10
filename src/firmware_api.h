// The table a firmware library hands to its loader.
//
// A library built with daisycola_add_firmware holds the firmware and its own copy of daisycola.
// It exports one function, daisycola_firmware_api(), which returns this table: host.h's calls
// as plain function pointers. daisycola_host (host_loader.cpp) implements host.h by calling
// through it. Nothing crosses the boundary that the library owns after the call: no exceptions
// (errors come back as text) and no pointers into the library.
#pragma once

#include <cstddef>
#include <cstdint>

#include "daisycola/host.h"

namespace daisycola::abi
{
// Bumped whenever the table changes. Both sides are built from the same sources, so a mismatch
// means a library from another daisycola build.
constexpr uint32_t kVersion = 1;

struct FirmwareApi
{
    uint32_t version;
    uint32_t size; // sizeof(FirmwareApi)

    // Firmware
    void (*start)();
    bool (*halt)(uint32_t timeout_ms);
    void (*wake)();
    BoardState (*get_board_state)();
    IrqStats (*get_irq_stats)(Irq irq);

    // Releases what the library holds in the process (timers, signal handlers, the SDRAM mapping,
    // the card's open files, the firmware thread) before it's unloaded. Writes why not and
    // returns false if it can't, while the firmware runs.
    bool (*shutdown)(char* error, size_t error_size);

    // Pins
    void (*set_pin)(daisy::Pin pin, bool level);
    void (*release_pin)(daisy::Pin pin);
    bool (*get_pin)(daisy::Pin pin);
    uint32_t (*get_pin_changes)(daisy::Pin pin);

    // CD4021 shift registers
    int (*attach_sr4021)(daisy::Pin clk, daisy::Pin latch, daisy::Pin data, int chips, int hold_reads);
    void (*set_sr_inputs)(int chain, uint64_t levels);
    void (*set_sr_input)(int chain, int bit, bool level);
    uint64_t (*get_sr_inputs)(int chain);
    int (*pending_sr_changes)(int chain);

    // Encoders
    int (*attach_encoder)(EncoderLine a, EncoderLine b, uint32_t dwell_us);
    void (*queue_detents)(int encoder, int detents);
    void (*queue_detents_on)(EncoderLine a, EncoderLine b, int detents);
    int (*pending_detents)(int encoder);

    // I2C
    void (*attach_i2c_device)(int bus, uint8_t address, I2CDevice* device);

    // Timer DMA
    bool (*get_dma_frame)(int timer, int channel, DmaFrame& frame);
    size_t (*ws2812_decode)(const uint32_t* duty,
                            size_t          count,
                            uint32_t        period,
                            uint32_t        prescaler,
                            ColorOrder      order,
                            Rgb*            leds,
                            size_t          max_leds,
                            double          reset_us);

    // Audio
    AudioFormat (*get_audio_format)();
    void (*set_audio_clock)(AudioClock clock);
    bool (*process_audio)(const float* const* in, float* const* out, size_t frames);
    size_t (*write_audio)(const float* const* in, size_t frames);
    size_t (*read_audio)(float* const* out, size_t frames);
    AudioStats (*get_audio_stats)();

    // MIDI
    size_t (*write_midi_in)(MidiPort port, const uint8_t* data, size_t size);
    size_t (*read_midi_out)(MidiPort port, uint8_t* data, size_t size);
    void (*set_usb_connected)(bool connected);

    // Single-threaded testing
    void (*use_manual_clock)(bool manual);
    void (*advance_clock)(uint64_t microseconds);
    size_t (*service_interrupts)();

    // SD card. Insert and eject write why not and return false where host.h throws.
    bool (*sd_insert)(const char* dir, char* error, size_t error_size);
    bool (*sd_eject)(char* error, size_t error_size);
    void (*sd_set_present)(bool present);
    bool (*sd_busy)();
};

} // namespace daisycola::abi

/** The library's entry point: returns its table. */
extern "C" const daisycola::abi::FirmwareApi* daisycola_firmware_api();
