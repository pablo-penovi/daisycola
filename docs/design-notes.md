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
the host snags fixed on the way: the force-included `daisycola/ff_integer.h`, the `f_size()`
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
sets its inputs with `SetSrInputs` (or holds them, see below). Input bit *i* is what the firmware reports as index *i*.

## Encoders step by reads and time, not by poll

CHOMPI's `ChompiEncoder` samples its lines at most once per millisecond, and its shift-register
decoder needs A low for two samples in a row. Stepping one Gray-code state per poll (2 kHz in TAPE)
would skip states. Stepping by time alone skips them too when reads come in bursts: with the host's
audio clock, the audio callback runs a host period's blocks back to back and then nothing until the
next period, so a 5 ms period can hide a whole 3 ms state. A skipped state makes TAPE drop the
detent or count it the wrong way.

So a state steps only once the firmware has read it in two different milliseconds and it has been
held for a dwell time (3 ms by default). The encoder's whole state, its phase, target and reads, is
one atomic word: the host's queue and the firmware's reads update it with compare-and-swap, so reads
stay lock-free and safe from interrupts.

## Shift-register inputs can hold each level

The same bursts drop key taps. The fork's CD4021 debounce counts at most one read a millisecond,
and TAPE wants 8 of them before a key counts as pressed. With a 1024-frame host period the audio
callback reads the keys in a burst every 21 ms, a millisecond or two of reads each, so a key has
to be held for 100 to 170 ms to register, and anything shorter vanishes.

`AttachSr4021` therefore takes an optional `hold_reads`. On such a chain each change to an input
is queued, and the firmware sees an input's next level only once it has read the current one in
`hold_reads` different milliseconds. Like the encoders, each input is one atomic word (level,
reads, the millisecond of the last read, changes queued) that the host and the firmware update
with compare-and-swap. A level nobody has read yet counts as read enough, so a change to a
resting input shows on the next read. The host's own view, `GetSrInputs`, still has the levels
it set. Holding is opt-in, because it delays every change behind the one before it.

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

## A blocking write waits for a running DMA read

libDaisy's `TransmitBlocking` spins until its peripheral is idle before it sends. TAPE's medium
battery check depends on that: it starts a DMA read of the MP2722 status and at once writes a new
BATT_LOW threshold, so on the chip the read sees the old threshold. daisycola's `TransmitBlocking`
waits for a DMA job on the same bus to finish, letting its interrupt run, before it writes. Without
that the write took 0.3 ms and the 6-byte read 0.6 ms, so the write landed first and a medium
battery read as high.

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

## Halting

`Halt` stops the timers and parks the firmware at its next delay or clock read outside a handler
(or in STOP mode). Parking jumps back to the start of the firmware thread with every signal
blocked, and the thread ends; `Halt` joins it. Nothing unwinds, so no destructor in the firmware
runs and nothing is torn down under it, and parking in main context means TAPE's timer callback
isn't halfway through an SD write. The jump is `_setjmp`/`_longjmp`, with the signals blocked by
hand first: ThreadSanitizer (GCC 16 with glibc 2.44) loses track of `sigsetjmp` and `setjmp` and
aborts on the jump back.

A halted firmware can't be started again in place. Its state is spread over globals: the
firmware's static buffers, libDaisy's and daisycola's own, function-local statics, constructors
that ran once. Loading its code again is the only reliable way to reset all of it, which is what a
power cycle does (below). Firmware linked into the program therefore runs once per process, and
the firmware tests rely on ctest running each test in its own process.

## The SD card is a folder

The card used to be a FAT32 disk image behind a FatFs block driver. To get samples in and out, a
host had to copy files through FatFs or loop-mount the image. Now the card is a host folder, and
daisycola implements FatFs's public API on it directly (`src/sys/ff_folder.cpp`); `ff.c`, the
driver layer and the image are gone.

The functions mirror FatFs R0.12c with libDaisy's configuration wherever a folder can show the
difference: names match without regard to case (exact name first, then a case-insensitive scan of
the directory), a rename doesn't replace an existing file (TAPE unlinks first), a seek past the
end of a writable file extends it, the string functions convert line ends (`_USE_STRFUNC` 2), and
`f_getcwd` puts the drive first because there are two volumes. A full disk returns a short count
from `f_write`, as FatFs does, and `FR_DENIED` elsewhere.

They run on the firmware thread and in TAPE's timer interrupt, so they can't allocate or lock: a
handler that interrupted `malloc` would deadlock. Paths are resolved in fixed buffers, directories
are read with the `getdents64` system call rather than `readdir` (which allocates, and whose
`DIR` would clash with FatFs's), and errors go to stderr with `write(2)`. Open files are a fixed
table of host descriptors keyed by the `FIL*`, claimed with compare-and-swap. `f_size`, `f_tell`
and `f_eof` are macros that read `obj.objsize` and `fptr` straight from the `FIL`, so every call
keeps those fields current.

The folder is opened once, at `SdInsert`, and every path is resolved relative to that descriptor
(`openat` and friends), so a later `chdir` in the host doesn't matter. A deleted folder stays open
with no links left; each call checks for that and answers `FR_NOT_READY`, as for a pulled card,
rather than writing into a directory nobody can see or recreating it.

## Power cycles

A firmware library (`daisycola_add_firmware`) holds the firmware, the libDaisy sources and its own
copy of daisycola. It exports a single function returning a table of the host API
(`src/firmware_api.h`); everything else is hidden. `daisycola_host` (`src/host_loader.cpp`) is the
host side: it implements `host.h` by forwarding through the table, so host code is the same either
way.

Unloading only resets the firmware if the library really goes away, and three things can stop
that. GNU unique symbols, which GCC uses for static locals of inline functions and template
statics, make `dlclose` a no-op, so everything is compiled with `-fno-gnu-unique`. A `thread_local`
the library touches on another thread pins it, or leaves a TLS block behind, so daisycola has none
(the firmware thread is recognised by `pthread_self`). And anything the library left registered
in the process would call into unmapped code: the POSIX timers, which are deleted; the signal
handlers, which are put back as they were; the SDRAM mapping, the card's descriptors and the
firmware thread, which is joined. After `dlclose`, `dlopen(RTLD_NOLOAD)` must find nothing, or
the cycle fails rather than run the firmware with stale state.

Host threads call in at any time, the audio thread included, so the loader can't take a lock. Each
call counts itself into an atomic in-flight counter and then reads the table pointer; an unload
clears the pointer first and waits for the counter to reach zero before releasing anything. A call
that finds no table does nothing: silence, no MIDI, zeros. `ProcessAudio` also produces silence on
a loaded board before the host has chosen the host clock, since a freshly loaded board starts with
the internal one while the host's audio thread keeps running.

The firmware is halted before anything else, with the library still in place, so a firmware that
won't halt is left running and the host can decide what to do.

## ThreadSanitizer changes signal timing

ThreadSanitizer defers asynchronous signals to its own safe points and runs the deferred handlers
with every signal blocked. Under it, interrupts don't nest and arrive a little late. Race detection
still works. The preemption test skips its nesting check under ThreadSanitizer. ThreadSanitizer
also keeps the shadow memory of an unloaded library resident (ld.so unmaps the library behind its
back), so the TAPE power-cycle test only checks resident memory without it.

## Raw SDRAM and AddressSanitizer

`Start` maps 64 MB at 0xC0000000 for firmware that uses raw SDRAM addresses, and a power cycle
unmaps it. On x86-64 that address lies in AddressSanitizer's shadow gap. With
`ASAN_OPTIONS=protect_shadow_gap=0`, older ASan versions leave the gap unmapped and count it as
ordinary memory, so daisycola maps the SDRAM and its (zeroed) shadow there. Newer ones (GCC 16)
map the whole gap read-write themselves; daisycola then uses the range in place and empties it
with `madvise(MADV_DONTNEED)` instead of unmapping it. The TAPE tests and the power-cycle tests
set the option through `__asan_default_options`. Without it,
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
tests check that value.

Negative samples need the SAI's 24-bit word. `s242f` sign-extends from bit 23 with
`(x ^ 0x800000) - 0x800000`, which is right for a raw 24-bit word but takes about 2.0 off a
negative int32 straight from `f2s24`. On the device the SAI only carries the low 24 bits, so
daisycola masks the word to 24 bits between the two conversions. Until CHAMPI's headless runner
looked at TAPE's output, no test had a negative sample, and every negative half-wave came out
near -2.0.

Audio buffers are arrays of `kMaxAudioChannels` pointers; a null pointer
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
