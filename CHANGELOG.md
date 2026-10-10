# Changelog

All notable changes to daisycola are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). The version itself lives in `VERSION.md`: bumping it on
`main` tags that commit from CI.

## [Unreleased]

## [0.1.0] - 2026-10-10

The first version: enough of libDaisy's hardware layer, backed by software, to run the CHOMPI TAPE
firmware unchanged on a Linux PC.

### Added

- **Build skeleton.** Headers with libDaisy's names and interfaces, so firmware sources compile
  and link unchanged for x86. `docs/headers.md` maps each libDaisy header to its daisycola
  counterpart.
- **Virtual SD card.** A FatFs disk driver backed by an image file. `SdCreateImage` formats a
  sparse FAT32 image, and `SdCopyIn`, `SdCopyOut`, `SdList` and `SdReadFile` move files in and out.
  The firmware reaches it through `FatFSInterface`, as on the device.
- **Peripheral models:**
  - a pin table behind `GPIO` and `dsy_gpio`, which hosts drive and read;
  - CD4021 shift-register chains modelled at the pin level, so `ShiftRegister4021` runs
    unchanged, debounce included;
  - quadrature encoders on pins or shift-register inputs;
  - `I2CHandle` with host device models, and DMA reads that complete in an interrupt;
  - `TimerHandle` and `TimChannel` DMA, with a WS2812 decoder that turns captured buffers into LED
    colours.
- **The virtual MCU.** The firmware's `main` runs on its own thread, and each interrupt line is a
  real-time signal to it, masked by NVIC priority, PRIMASK and the NVIC enables. POSIX timers drive
  `Schedule` and `SetPeriodic`, `Halt` parks the thread, and STOP mode waits for the host's
  `Wake`. A single-threaded mode with a manual clock serves the tests.
- **Audio and MIDI.** `AudioHandle`, `SaiHandle` and `DaisySeed` run the firmware's callback from
  an internal clock or from the host's `ProcessAudio`, with libDaisy's own sample conversions. UART
  and USB MIDI go through libDaisy's parser via byte rings. SDRAM is mapped at `0xC0000000`, also
  under ASan.
- `AttachSr4021` takes an optional `hold_reads`: each change to an input is held until the
  firmware has read it in that many different milliseconds, so a quick tap survives bursty reads.
  `PendingSrChanges` reports the changes still queued. The default of 0 keeps the old behaviour
  ([#3](https://github.com/pablo-penovi/daisycola/pull/3)).
- A guide to using daisycola and adding peripherals.
- A README with a full disclosure section
  ([#1](https://github.com/pablo-penovi/daisycola/pull/1)).

### Fixed

- I2C `TransmitBlocking` now waits for a running DMA read on the same bus, as libDaisy's does.
  Writing straight away broke TAPE's medium-battery check.
- Negative 24-bit samples came back about 2.0 too low. They're masked to 24 bits before
  sign extension, as the SAI does on the device.
- Encoders dropped or reversed detents when the firmware read them in bursts, as it does with the
  host's audio clock. A state now steps only once it has been read in two different milliseconds
  and held for the dwell.
- `f_size()` evaluates as on the STM32: integer arithmetic wraps at 32 bits, and a product with a
  float converts to `uint32_t` with saturation. On x86-64, TAPE's sample windows computed before
  a file opened wrapped to 0, so a cubbi voice's first press was silent
  ([#2](https://github.com/pablo-penovi/daisycola/pull/2)).

[Unreleased]: https://github.com/pablo-penovi/daisycola/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/pablo-penovi/daisycola/tree/v0.1.0
