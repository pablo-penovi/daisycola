/* daisycola host API: how a host program drives the virtual Daisy board.
 *
 * The firmware runs on its own thread. Everything here is meant to be called from host threads
 * unless it says otherwise.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "daisy_core.h"

namespace daisycola
{
// ---- Firmware ----------------------------------------------------------------------------------

/** The firmware's main function, renamed at compile time with -Dmain=<name>. */
using FirmwareMain = int (*)();

/** Starts the firmware on its own thread. */
void Start(FirmwareMain firmware_main);

// ---- Pins -------------------------------------------------------------------------------------
//
// Levels are electrical: true is high. A pin the host has never driven reads its pull-up or
// pull-down, or 0 with no pull.

/** Drives an input pin from outside, as a switch or another chip would. */
void SetPin(daisy::Pin pin, bool level);

/** Stops driving a pin: it reads its pull again. */
void ReleasePin(daisy::Pin pin);

/** The pin's level: what the firmware drives on an output, or what it reads on an input. */
bool GetPin(daisy::Pin pin);

/** How many times the firmware has changed an output pin's level. */
uint32_t GetPinChanges(daisy::Pin pin);

// ---- CD4021 shift registers --------------------------------------------------------------------
//
// A chain of CD4021 parallel-in/serial-out registers on a clock, latch and data pin. The
// firmware's own ShiftRegister4021 code bit-bangs the pins and the model answers, so its debounce
// and edge logic run unchanged.
//
// Input bit 8*k + (n-1) is parallel input Pn of chip k. Chips are counted from the start of the
// chain, the chip farthest from the MCU; the last chip drives the data pin. With this numbering,
// bit i is what ShiftRegister4021 reports as index i. All inputs start high.

/** Wires a chain of `chips` CD4021s (1 to 8) to the pins. Returns the chain's id. */
int AttachSr4021(daisy::Pin clk, daisy::Pin latch, daisy::Pin data, int chips);

/** Sets the levels on all of a chain's parallel inputs. */
void SetSrInputs(int chain, uint64_t levels);

/** Sets one parallel input. */
void SetSrInput(int chain, int bit, bool level);

/** The levels last set on a chain's inputs. */
uint64_t GetSrInputs(int chain);

// ---- Quadrature encoders -----------------------------------------------------------------------
//
// An encoder's A and B lines can be pins or shift-register inputs. Both rest high. A queued detent
// steps the lines through one full Gray-code cycle, holding each state for `dwell_us`. A positive
// detent is the direction the firmware's encoder code counts as +1 (B falls before A).

/** One encoder line: a pin, or input `bit` of a CD4021 chain. */
struct Line
{
    static Line OnPin(daisy::Pin pin) { return {pin, -1, 0}; }
    static Line OnSr(int chain, int bit) { return {daisy::Pin(), chain, bit}; }

    daisy::Pin pin;
    int        chain; // -1 for a pin
    int        bit;
};

/** Wires a quadrature encoder to two lines. Returns the encoder's id.
 *
 *  The dwell must cover at least two of the firmware's reads: CHOMPI samples encoders once per
 *  millisecond, and its shift-register decoder needs A low for two samples. The 3 ms default
 *  gives about 80 detents per second. */
int AttachEncoder(Line a, Line b, uint32_t dwell_us = 3000);

/** Queues detents on an encoder: positive is +1 for the firmware, negative is -1. */
void QueueDetents(int encoder, int detents);

/** Finds the encoder on these lines, wiring one up first if there is none, and queues detents. */
void QueueDetents(Line a, Line b, int detents);

/** Detents queued on the encoder that haven't finished yet. */
int PendingDetents(int encoder);

// ---- I2C devices -------------------------------------------------------------------------------
//
// A device model answers transfers on a bus. Its methods run on the firmware's thread, sometimes
// inside an interrupt, so they must not lock, allocate or block.

class I2CDevice
{
  public:
    virtual ~I2CDevice() = default;

    /** The firmware wrote `size` bytes. Return false to NACK. */
    virtual bool Write(const uint8_t* data, size_t size) = 0;

    /** The firmware reads `size` bytes. Return false to NACK. */
    virtual bool Read(uint8_t* data, size_t size) = 0;
};

/** Puts a device at a 7-bit address on a bus (0 = I2C_1 ... 3 = I2C_4). Null removes it. */
void AttachI2CDevice(int bus, uint8_t address, I2CDevice* device);

// ---- Timer PWM DMA (WS2812 LEDs) ---------------------------------------------------------------

constexpr size_t kMaxDmaWords = 4096;

/** The buffer a TimChannel DMA transfer sent, captured when the transfer completed. */
struct DmaFrame
{
    int      timer;     // 2..5 for TIM2..TIM5
    int      channel;   // 1..4
    uint64_t sequence;  // 1 for the first transfer on this channel, then counts up
    uint64_t time_us;   // when the transfer completed
    uint32_t prescaler; // timer prescaler register (PSC)
    uint32_t period;    // timer auto-reload register (ARR): a bit lasts period + 1 ticks
    uint32_t count;     // words in duty
    uint32_t duty[kMaxDmaWords];
};

/** Copies the newest frame sent on a timer channel. False if nothing was sent yet. */
bool GetDmaFrame(int timer, int channel, DmaFrame& frame);

/** Byte order of a WS2812-style LED. */
enum class ColorOrder
{
    kGrb,
    kRgb,
};

struct Rgb
{
    uint8_t r, g, b;
};

/** Decodes a PWM duty buffer the way a WS2812 chain reads it.
 *
 *  A word with a pulse is one bit, MSB first: 1 if the pulse is longer than half the bit period,
 *  0 if shorter. A word with no pulse (duty 0) holds the line low; low for reset_us or more
 *  latches the chain, and the LEDs after it start again from the first. Returns the number of
 *  complete LEDs after the last reset, writing up to max_leds of them. */
size_t Ws2812Decode(const uint32_t* duty,
                    size_t          count,
                    uint32_t        period,
                    uint32_t        prescaler,
                    ColorOrder      order,
                    Rgb*            leds,
                    size_t          max_leds,
                    double          reset_us = 50.0);

/** Ws2812Decode on a captured frame. */
size_t Ws2812Decode(const DmaFrame& frame, ColorOrder order, Rgb* leds, size_t max_leds);

// ---- Single-threaded testing -------------------------------------------------------------------
//
// Without the firmware thread, daisycola's peripherals can be driven straight from a test.
// Interrupts are queued and run by ServiceInterrupts on the calling thread.

/** Switches to a clock that only moves through AdvanceClock and firmware delays. */
void UseManualClock(bool manual);

/** Moves the clock forward, running interrupts as they come due. */
void AdvanceClock(uint64_t microseconds);

/** Runs every pending or due interrupt. Returns how many ran. */
size_t ServiceInterrupts();

// ---- SD card -----------------------------------------------------------------------------------
//
// The card is a disk-image file: an MBR with one FAT32 partition, like a real microSD card. It
// can be loop-mounted or used with mtools. The helpers below format, fill and read the image
// through FatFs on the calling thread. They must not run while the firmware is running.

/** Thrown by the SD-card helpers. */
class SdError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

/** Creates a sparse image file of `size_bytes` and formats it as FAT32. Overwrites `path`. */
void SdCreateImage(const std::string& path, uint64_t size_bytes);

/** Inserts the card: the firmware reads and writes this image file from now on. */
void SdOpenImage(const std::string& path);

/** Removes the card and closes the image file. */
void SdCloseImage();

/** Simulates pulling the card out (false) or putting it back (true) without closing the image. */
void SdSetPresent(bool present);

/** True while the firmware is in the middle of a card read or write. */
bool SdBusy();

/** One file or directory on the card. */
struct SdEntry
{
    std::string name;
    uint64_t    size;
    bool        is_dir;
};

/** Lists a directory on the card ("/" for the root). */
std::vector<SdEntry> SdList(const std::string& card_dir = "/");

/** Copies a host file, or a host directory recursively, into a directory on the card. */
void SdCopyIn(const std::string& host_path, const std::string& card_dir = "/");

/** Copies a card file, or a card directory recursively, into a host directory. */
void SdCopyOut(const std::string& card_path, const std::string& host_dir);

/** Reads a whole file from the card. */
std::vector<uint8_t> SdReadFile(const std::string& card_path);

} // namespace daisycola
