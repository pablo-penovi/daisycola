// The virtual MCU: firmware thread, interrupt lines, interrupt masking and the clock.
//
// On the STM32 the firmware's main loop is preempted by interrupt handlers on a single core. Here
// the firmware runs on one thread, and each interrupt line is a real-time signal sent to that
// thread, so handlers preempt the main loop on its own stack just as on the chip. Signal
// numbers and handler masks follow the lines' NVIC priorities.
//
// Before the firmware thread starts (and in single-threaded tests) the same calls work without
// signals: interrupts are queued and ServiceInterrupts() runs them on the calling thread.
#pragma once

#include <cstddef>
#include <cstdint>

#include "stm32h7xx.h"

namespace daisycola::mcu
{
/** Interrupt lines, highest priority first. */
enum class Line : int
{
    kAudio,  // SAI DMA, NVIC priority 0
    kI2c,    // I2C DMA transfer complete, priority 0
    kUart,   // UART receive, priority 0
    kTimDma, // TIM PWM DMA transfer complete (WS2812 LEDs), priority 0
    kUsb,    // USB OTG, priority 0
    kTim2,   // timer period interrupts, priority 15
    kTim3,
    kTim4,
    kTim5,
    kCount,
};

constexpr int kLineCount = int(Line::kCount);

using Handler = void (*)(void* context);

/** Sets the function that runs when the line's interrupt fires. */
void SetHandler(Line line, Handler handler, void* context);

/** Sets the NVIC interrupt number that HAL_NVIC_EnableIRQ/DisableIRQ use for the line. */
void SetIrqNumber(Line line, IRQn_Type irqn);

/** Makes the line's interrupt pending now. Callable from any thread. */
void Raise(Line line);

/** Makes the line's interrupt pending after a delay, replacing an earlier Schedule. */
void Schedule(Line line, uint64_t delay_ns);

/** Fires the line every period_ns; 0 stops it. */
void SetPeriodic(Line line, uint64_t period_ns);

/** True while an interrupt handler runs (on the firmware thread or in ServiceInterrupts). */
bool InInterrupt();

// ---- PRIMASK and NVIC ---------------------------------------------------------------------------

void     DisableIrq();
void     EnableIrq();
uint32_t GetPrimask();
void     SetPrimask(uint32_t primask);
void     NvicSetEnabled(IRQn_Type irqn, bool enabled);

/** Blocks every interrupt without touching PRIMASK. Used while the device sleeps. */
void SetAllMasked(bool masked);

// ---- Clock --------------------------------------------------------------------------------------

/** Nanoseconds since the board booted. */
uint64_t NowNs();

/** Waits until NowNs() >= t_ns, letting interrupts run meanwhile. */
void SleepUntil(uint64_t t_ns);

/** Switches to a clock that only moves when AdvanceClock (or a firmware delay) moves it. Only
 *  allowed before the firmware thread starts. */
void UseManualClock(bool manual);

/** Moves the manual clock forward and runs the interrupts that became due on the way. */
void AdvanceClock(uint64_t ns);

/** Runs pending and due interrupts on the calling thread, highest priority first, until none
 *  are left. Only for when the firmware thread is not running. Returns how many ran. */
size_t ServiceInterrupts();

// ---- Firmware thread ----------------------------------------------------------------------------

/** Starts the firmware's main on its own thread. From then on interrupts are signals. */
void StartFirmware(int (*firmware_main)());

/** True once the firmware thread has started, until it halts or main returns. */
bool FirmwareRunning();

/** True once StartFirmware has been called. The firmware can only start once: running it again
 *  needs a fresh copy of its code (a firmware library, power-cycled). */
bool FirmwareStarted();

/** Stops the timers and parks the firmware at its next delay or clock read outside an
 *  interrupt, which ends its thread. Returns false if that didn't happen within the timeout. */
bool HaltFirmware(uint32_t timeout_ms);

/** Releases what the MCU holds in the process, before a firmware library is unloaded: deletes
 *  the timers, joins the firmware thread and puts back the signal handlers that were there before
 *  StartFirmware. Returns false, doing nothing, while the firmware runs. */
bool Shutdown();

/** STOP mode: masks every interrupt and waits for Wake(). Without the firmware thread it returns
 *  at once, as if woken immediately. */
void EnterStop();

/** Ends STOP mode. Callable from any thread. */
void Wake();

struct FirmwareState
{
    bool     started;
    bool     running;
    bool     sleeping;
    bool     exited;    // main returned
    int      exit_code; // what it returned
    uint64_t sleeps;    // times STOP mode was entered
};
FirmwareState GetFirmwareState();

/** Runs a short section with every interrupt masked, from main or interrupt context, and
 *  restores PRIMASK afterwards. */
class Critical
{
  public:
    Critical() : saved_(GetPrimask()) { DisableIrq(); }
    ~Critical() { SetPrimask(saved_); }
    Critical(const Critical&) = delete;
    Critical& operator=(const Critical&) = delete;

  private:
    uint32_t saved_;
};

/** Per-line interrupt statistics. */
struct LineStats
{
    uint64_t count;    // handler runs
    uint64_t total_ns; // time spent in the handler
    uint64_t max_ns;   // longest single run
    uint64_t dropped;  // raises lost because the signal queue was full
};
LineStats GetLineStats(Line line);

} // namespace daisycola::mcu
