/* daisycola host API: how a host program drives the virtual Daisy board.
 *
 * This is the only daisycola header a host program needs. The order of things:
 *
 *   1. Wire the board: CD4021 chains, encoders, I2C devices, pins, the SD card image, the audio
 *      clock. Wiring calls are only allowed before Start.
 *   2. Start the firmware. It runs on its own thread, with interrupts as on the device.
 *   3. Drive inputs and read outputs from host threads: pins, shift-register inputs, encoder
 *      detents, audio, MIDI, LED frames.
 *   4. Halt it, if the host wants to look at the SD card or exit cleanly.
 *
 * Everything here is meant to be called from host threads unless it says otherwise. Calls marked
 * "from one thread" must not be made from two threads at once.
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
//
// Set the board up first (chips, encoders, I2C devices, the SD card, the audio clock), then start
// the firmware. A process runs firmware once: restarting it needs a new process.

/** The firmware's main function, renamed at compile time with -Dmain=<name>. */
using FirmwareMain = int (*)();

/** Starts the firmware on its own thread and returns once it is running. */
void Start(FirmwareMain firmware_main);

/** Stops the firmware: its timers stop and the thread parks for good at its next delay or clock
 *  read outside an interrupt handler. Returns false if that didn't happen within the timeout.
 *  Afterwards the SD-card helpers can be used again. */
bool Halt(uint32_t timeout_ms = 1000);

/** Wakes the firmware from STOP mode (HAL_PWR_EnterSTOPMode), as a wake-up interrupt would. */
void Wake();

struct BoardState
{
    bool     started;   // Start was called
    bool     running;   // the firmware thread runs: started, not halted, main hasn't returned
    bool     sleeping;  // in STOP mode, waiting for Wake
    bool     exited;    // main returned
    int      exit_code; // what main returned
    uint64_t sleeps;    // times the firmware entered STOP mode
};

BoardState GetBoardState();

/** Interrupt sources of the virtual MCU, highest priority first. Audio, I2C, UART, the timer DMA
 *  and USB have NVIC priority 0 and don't preempt each other; the timers have priority 15. */
enum class Irq
{
    kAudio,
    kI2c,
    kUart,
    kTimDma,
    kUsb,
    kTim2,
    kTim3,
    kTim4,
    kTim5,
};

struct IrqStats
{
    uint64_t count;    // handler runs
    uint64_t total_ns; // time spent in the handler
    uint64_t max_ns;   // longest single run
    uint64_t dropped;  // raises lost because the signal queue was full
};

IrqStats GetIrqStats(Irq irq);

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
// steps the lines through one full Gray-code cycle. Each state holds for `dwell_us` and until the
// firmware has read it in two different milliseconds, so reads in bursts see every state. A positive
// detent is the direction the firmware's encoder code counts as +1 (B falls before A).

/** One encoder line: a pin, or input `bit` of a CD4021 chain. */
struct EncoderLine
{
    static EncoderLine OnPin(daisy::Pin pin) { return {pin, -1, 0}; }
    static EncoderLine OnSr(int chain, int bit) { return {daisy::Pin(), chain, bit}; }

    daisy::Pin pin;
    int        chain; // -1 for a pin
    int        bit;
};

/** Wires a quadrature encoder to two lines. Returns the encoder's id.
 *
 *  CHOMPI samples encoders once per millisecond, and its shift-register decoder needs A low for
 *  two samples. The 3 ms default gives about 80 detents per second when the firmware reads every
 *  millisecond; fewer when its reads are further apart. */
int AttachEncoder(EncoderLine a, EncoderLine b, uint32_t dwell_us = 3000);

/** Queues detents on an encoder: positive is +1 for the firmware, negative is -1. */
void QueueDetents(int encoder, int detents);

/** Finds the encoder on these lines, wiring one up first if there is none, and queues detents. */
void QueueDetents(EncoderLine a, EncoderLine b, int detents);

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

// ---- Audio -------------------------------------------------------------------------------------
//
// The firmware's audio callback runs in the audio interrupt, one block at a time (TAPE: 24 frames
// of 4 channels at 48 kHz). Buffers are planar: arrays of kMaxAudioChannels channel pointers, in
// the order the callback sees the channels. A null input channel is silence and a null output
// channel is skipped; output channels the firmware doesn't have are silent. Samples pass through
// the codec's integer format as on the device: input and output are clipped to +-0.999985 and
// quantised to the SAI's bit depth, and the output is scaled by postgain and output_compensation,
// as libDaisy's AudioHandle does.

constexpr size_t kMaxAudioChannels = 4;

struct AudioFormat
{
    bool   started;     // the firmware has started audio; the rest is valid from then on
    float  sample_rate; // Hz
    size_t block_size;  // frames per callback
    size_t channels;    // 2 with one SAI, 4 with two
};

AudioFormat GetAudioFormat();

/** What runs the audio interrupt. */
enum class AudioClock
{
    kInternal, // a timer at the block rate, like the codec's clock; the default
    kHost,     // the host's audio thread, through ProcessAudio
};

/** Chooses the audio clock. Call before Start. */
void SetAudioClock(AudioClock clock);

/** With the host clock: hands the firmware `frames` frames of input, runs the audio interrupt
 *  for every full block, and returns `frames` frames of output. Output lags input by two blocks.
 *  Call from one thread (the host's audio thread). Waits for
 *  the firmware for a bounded time; returns false, padding with silence, if it fell behind or
 *  hasn't started audio yet. */
bool ProcessAudio(const float* const* in, float* const* out, size_t frames);

/** With the internal clock: queues input for the firmware. Returns the frames that fit; blocks
 *  that find no input get silence. Call from one thread. */
size_t WriteAudio(const float* const* in, size_t frames);

/** With the internal clock: reads output the firmware has produced. Returns the frames read.
 *  Output that isn't read is dropped once the buffer (8192 frames) is full. Call from one
 *  thread. */
size_t ReadAudio(float* const* out, size_t frames);

struct AudioStats
{
    uint64_t blocks;    // audio callbacks run
    uint64_t underruns; // blocks that ran without (enough) input
    uint64_t overruns;  // output blocks dropped because the host didn't read them
};

AudioStats GetAudioStats();

// ---- MIDI --------------------------------------------------------------------------------------
//
// Raw MIDI bytes in and out of the firmware's UART (TRS/DIN, USART1) and USB MIDI ports. Input
// raises the port's receive interrupt, so the firmware's MidiHandler parses it as on the device.
// Each function may be called from any host thread.

enum class MidiPort
{
    kUart,
    kUsb,
};

/** Sends bytes to the firmware. Returns how many fit (the buffer holds 4096). USB input is
 *  dropped while USB is disconnected. */
size_t WriteMidiIn(MidiPort port, const uint8_t* data, size_t size);

/** Takes bytes the firmware sent. Returns how many were read. */
size_t ReadMidiOut(MidiPort port, uint8_t* data, size_t size);

/** Plugs in or unplugs USB. While unplugged (the default), the firmware's USB sends fail. */
void SetUsbConnected(bool connected);

// ---- Single-threaded testing -------------------------------------------------------------------
//
// Before the firmware thread starts, daisycola's peripherals can be driven straight from a test.
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
