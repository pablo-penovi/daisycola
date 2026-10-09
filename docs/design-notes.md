# Design notes

Decisions made while building daisycola, where they differ from the original plan or aren't
obvious from the code.

## Fewer replacement headers

The plan had daisycola replace every libDaisy header TAPE touches. Most of them only declare
things, so they are used as they are and daisycola defines the bodies in `src/` (GPIO,
TimerHandle, TimChannel, I2C, SAI, Audio, UART, MidiUsbTransport, System, Sdmmc,
FatFSInterface). Only `daisy.h`, `daisy_seed.h`, `daisy_core.h` (an `#include_next` wrapper),
`dev/sdram.h`, `stm32h7xx.h`, `stm32h7xx_hal.h`, `cmsis_gcc.h` and `Limiter.h` are replaced.
Headers that libDaisy reaches by relative path (`sys/system.h`, `util/scopedirqblocker.h`,
`util/FIFO.h`) can't be replaced at all. [headers.md](headers.md) lists each one, along with
the host snags fixed on the way: the force-included `daisycola/ff_integer.h`, the `f_write`
wrapper and the DaisySP `DelayLine` overload. Because of that overload, a firmware build must put
daisycola's include directories before DaisySP's.

## Missing APIs fail loudly

daisycola only implements what CHOMPI firmware uses. Firmware that calls anything else gets a
link error rather than a body that silently does nothing. The few bodies still left as stubs
(`src/stub.h`) abort with a message when called.

## Chips are modelled at the pin level

`dev/sr_4021.h` is used unchanged. The firmware's `ShiftRegister4021::Update()` bit-bangs clock,
latch and data through `dsy_gpio`, and daisycola's CD4021 model answers on those pins: P/S high
loads the parallel inputs, a rising clock shifts towards the data pin. The fork's debounce and edge
code therefore runs as written; nothing is copied. The host wires a chain with `AttachSr4021` and
sets its inputs with `SetSrInputs`. Input bit *i* is what the firmware reports as index *i*.

## Encoders step by time, not by poll

CHOMPI's `ChompiEncoder` samples its lines at most once per millisecond, and its shift-register
decoder needs A low for two samples in a row. Stepping one Gray-code state per poll (2 kHz in TAPE)
would skip states. Instead each state is held for a dwell time (3 ms by default), and the line
levels are a pure function of the clock and one atomic word the host writes. Reads are lock-free
and safe from interrupts.

## WS2812 bits: longer than half a bit is 1

The plan said "duty ≥ ⅔ of the period is a 1". TAPE's timing is 20 ticks for a 1 and 10 for a 0
out of a 36-tick bit (0.67 µs and 0.33 µs of 1.2 µs), so ⅔ would read every bit as 0. The decoder
uses half the bit period, which is close to where a WS2812 samples. Words with no pulse at all
(duty 0, TAPE's "porch" slots) aren't bits: they hold the line low, and 50 µs of low latches the
chain. So the decoder returns exactly the 25 key LEDs and 10 PTH LEDs, without the host having to
know about porches.

## Timer rates follow the registers

Timer periods come from the prescaler and auto-reload values the firmware writes, at the STM32's
timer clock (2 × PCLK1: 240 MHz boosted, 200 MHz otherwise). TIM3 and TIM4 are 16-bit, and libDaisy
truncates the period it writes to them. TAPE asks TIM4 for 1 kHz with a period of 120000 ticks;
truncated to 16 bits that is 54464, so on the device (and here) its SD-card callback runs at about
4.4 kHz.

## Interrupt priorities follow the NVIC

libDaisy gives every DMA stream, I2C and UART interrupt NVIC priority 0 and the timers priority 15.
So audio, LED DMA and I2C completions can't preempt each other, and all of them preempt the TIM4
callback. The plan's "audio, then TIM4, then the rest" put the LED and I2C interrupts below TIM4;
daisycola follows the chip.

## I2C addresses

The HAL shifts the address left one bit and the peripheral sends bits 7:1, so only the low seven
bits of the address the firmware passes reach the bus. TAPE reads the MP2722 at `0x3F | 0x80`; the
device model is reached at 0x3F.

## Interrupts are real-time signals

The firmware's `main` runs on its own thread (16 MB stack). Each interrupt line is a real-time
signal, `SIGRTMIN + 2 + line`, sent to that thread, so a handler preempts the main loop on its own
stack as on the chip. Lines are numbered by priority, and Linux delivers the lowest-numbered
pending real-time signal first, so interrupts that are pending together run in the NVIC's order.
Each handler's `sa_mask` holds the lines its priority keeps out: priority-0 handlers mask every
line, the priority-15 timers mask the timers.

PRIMASK, NVIC enables and STOP mode are kept as state, and every change recomputes the thread's
signal mask from it (blocking all lines first, so that no handler sees a half-updated mask). When a
handler returns, the interrupted context's mask is recomputed too and written into the signal's
`ucontext`, so an NVIC change made inside a handler sticks, and `__enable_irq` inside a handler
goes back to the handler's level rather than unmasking everything. Handlers save and restore
`errno`.

`Schedule` and `SetPeriodic` use one POSIX timer per line, aimed at the firmware thread
(`SIGEV_THREAD_ID`). A periodic timer whose signal is still pending doesn't queue another, which is
how an NVIC pending bit behaves. Host threads raise lines with `pthread_sigqueue`. Raises can also
merge (ThreadSanitizer merges them), so daisycola's own handlers drain everything waiting rather
than one item per raise. Delays sleep with `clock_nanosleep`; a signal cuts the sleep short, its
handler runs, and the sleep resumes. The thread's timer slack is 1 ns.

The plan kept a fallback (one lock and an interrupt-runner thread) in case signals proved
unworkable. They didn't, so it doesn't exist.

## Halting, and one firmware per process

`Halt` stops the timers and parks the firmware thread for good at its next delay or clock read
outside a handler, with every signal blocked. It never unwinds, so nothing is torn down under the
firmware, and parking in main context means TAPE's timer callback isn't halfway through an SD
write. A parked firmware can't be restarted, so a process runs firmware once. The firmware tests
rely on ctest running each test in its own process.

## ThreadSanitizer changes signal timing

ThreadSanitizer defers asynchronous signals to its own safe points and runs the deferred handlers
with every signal blocked. Under it, interrupts don't nest and arrive a little late. Race detection
still works. The preemption test skips its nesting check under ThreadSanitizer.

## Raw SDRAM and AddressSanitizer

`Start` maps 64 MB at 0xC0000000 for firmware that uses raw SDRAM addresses. On x86-64 that address
lies in AddressSanitizer's shadow gap. With `ASAN_OPTIONS=protect_shadow_gap=0` the gap is left
unmapped and ASan counts it as ordinary memory, so daisycola maps the SDRAM and its (zeroed)
shadow there. The TAPE boot test sets the option through `__asan_default_options`. Without it,
`Start` warns and firmware that touches raw SDRAM crashes.

The option belongs to the executable, not the library. ASan reads its options when the process
starts, before `Start` could act, and `__asan_default_options` is one hook per program: if
daisycola defined it, it would clash with a host's own or silently override it. So a host's ASan
build defines the hook itself or runs with the environment variable. An opt-in
`daisycola::asan_sdram` CMake target holding only the hook would be a convenience; it isn't built,
and the choice waits until CHAMPI has an ASan build.

## Audio

The audio interrupt takes a block from the input ring, runs it through libDaisy's own conversions
(`f2s24`, then `s242f`: clipped to ±0.999985 and quantised to 24 bits), divides by `postgain`,
calls the callback, and converts the output back the same way after scaling by `postgain` and
`output_compensation`. Channels are in the order the callback sees them. For TAPE that is mic,
unused, aux L, aux R in, and headphones L/R, master L/R out.

Full scale doesn't wrap. In 24-bit two's complement, 1.0 × 2^23 = 0x800000 would read back as
-1.0, and the first audio test expected that. But `f2s24` clamps to ±`FBIPMAX` (0.999985) before
scaling, so 1.0 becomes about 8388482. Because daisycola uses libDaisy's own conversions both ways,
firmware output never goes past ±0.999985 and host input is clipped to the same range. The audio
tests check that value. Audio buffers are arrays of `kMaxAudioChannels` pointers; a null pointer
means an unused channel.

Two clocks can raise the interrupt. The internal clock is a timer at the block rate, which is what
headless runs use. With the host clock, the host's audio thread calls `ProcessAudio`; it raises
the interrupt once for every full block and waits (bounded) for the output. The output ring starts
with two blocks of silence, so host buffers that aren't a multiple of the block size always find
enough output.

## MIDI

Host bytes go into a ring and raise the port's receive interrupt. The UART handler copies them
into the firmware's circular DMA buffer and calls the listener, like libDaisy's idle-line handler;
the USB handler passes them to the parse callback. Either way the fork's own `MidiHandler` and
parser see them. `PollTx` takes the bytes' time on the wire (320 µs each at 31250 baud). USB sends
fail while the host has USB unplugged, after the fork's three retries 100 µs apart. Output is
written with interrupts masked, so a message sent from a handler can't split one sent from the main
loop.

## STOP mode

`HAL_PWR_EnterSTOPMode` masks every interrupt and waits for the host's `Wake()`. The clock keeps
running, unlike the chip's timers, so `System::GetNow()` jumps over the sleep.

## Single-threaded mode

Before the firmware thread exists, interrupts are queued and `ServiceInterrupts()` runs them on
the calling thread. With `UseManualClock(true)` time only moves through `AdvanceClock()` and
firmware delays, which step from one due interrupt to the next. The peripheral tests use this to
drive TAPE's own code deterministically. Once the firmware has started, the process stays in
thread mode.

## What TAPE needs to boot headless

TAPE's `MainLoop` waits on the battery charger and its interrupt line. A headless boot
needs a fake MP2722 at I2C 0x3F with VIN_GD (register 0x12, bit 6) set, `mpc_int` (D31) held high,
and both CD4021 chains attached with their inputs high. Without those it waits
forever.

High is the resting level on both chains. The keys are active-low: the fork's `State(i)` is true
only once the debounce counter has fallen below `dbc_size`, which takes the input held low, and
TAPE's UI treats `RisingEdge` of `State` as a press. So all-high means no key or encoder button is
pressed. The encoder chain carries encoders 1–4's A/B lines, which `ChompiEncoder` starts at
all-high and steps on falling edges, so all-high is also an encoder at rest. `tests/tape/boot_test.cpp` sets this up; it boots in about 1.6 s.

## Things the plan listed that CHOMPI doesn't use

- `SCB_CleanDCache_by_Addr` and friends: no CHOMPI firmware calls them. Cache maintenance that
  libDaisy does internally (`dsy_dma_clear_cache_for_buffer`) is a no-op.
- `reset_requested`: no CHOMPI firmware resets itself or jumps to the bootloader.
- The event log (a ring of pin changes, DMA frames and MIDI bytes for golden tests): optional in the
  plan, and nothing needs it yet. `GetPinChanges`, `GetDmaFrame` and the MIDI and audio rings cover
  what CHAMPI and the tests look at.
