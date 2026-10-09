# Design notes

Decisions made while building daisycola, where they differ from the original plan or aren't
obvious from the code.

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

## Single-threaded mode

Before the firmware thread exists, interrupts are queued and `ServiceInterrupts()` runs them on
the calling thread. With `UseManualClock(true)` time only moves through `AdvanceClock()` and
firmware delays, which step from one due interrupt to the next. The peripheral tests use this to
drive TAPE's own code deterministically.

## Things the plan listed that CHOMPI doesn't use

- `SCB_CleanDCache_by_Addr` and friends: no CHOMPI firmware calls them. Cache maintenance that
  libDaisy does internally (`dsy_dma_clear_cache_for_buffer`) is a no-op.
- `reset_requested`: no CHOMPI firmware resets itself or jumps to the bootloader.
