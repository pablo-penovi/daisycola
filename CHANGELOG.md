# Changelog

All notable changes to daisycola are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). The version itself lives in `VERSION.md`: bumping it on
`main` tags that commit from CI.

## [Unreleased]

## [0.2.0] - 2026-10-10

The SD card is a folder on the host, and firmware built as a shared library can be power-cycled
without restarting the host program. Breaking: the disk-image card and its helpers are gone.

### Added

- **Folder SD card.** `SdInsert(dir)` inserts a host folder as the card and `SdEject` removes it.
  daisycola implements FatFs's whole public API on the folder (`src/sys/ff_folder.cpp`),
  following FatFs's behaviour where a folder can show it: names match without regard to case, a
  rename doesn't replace a file, `..` can't leave the folder, and the string functions convert
  line ends. `disk_status` reports a missing card. A deleted folder reads as a pulled card
  (`FR_NOT_READY`) and is never recreated.
- **Power cycles.** `daisycola_add_firmware(<name> SOURCES ...)` builds firmware, libDaisy and
  daisycola into a loadable library, and the host links `daisycola_host` instead of `daisycola`.
  `LoadFirmware` loads it, `Start()` runs it, and `PowerCycle` halts it, releases everything it
  holds in the process (timers, signal handlers, the SDRAM mapping, card files, its thread),
  unloads it, checks that it's gone, and loads it again. `UnloadFirmware` does the first half.
  Errors are `FirmwareError`s. Host calls made while no library is loaded do nothing: audio is
  silent. `daisycola_firmware_library` prepares a static library that goes into a firmware
  library.
- Tests for every FatFs call on a folder, for power cycles (globals start from scratch, nothing is
  left behind, memory stays flat, audio stays silent in between, a firmware that won't halt keeps
  running), and TAPE power-cycled 20 times in one process.

### Changed

- `Halt` ends the firmware thread and joins it, rather than parking it forever. Nothing in the
  firmware unwinds, as before.
- `ProcessAudio` produces silence, instead of aborting, while the internal clock is still selected
  and the firmware hasn't started: a power-cycled board starts with it while the host's audio
  thread keeps running.
- Under newer AddressSanitizer versions, which map the shadow gap themselves, the SDRAM at
  0xC0000000 is used in place, without the misleading "needs protect_shadow_gap=0" warning.

### Removed

- The disk-image card: `SdCreateImage`, `SdOpenImage`, `SdCloseImage`, `SdList`, `SdCopyIn`,
  `SdCopyOut`, `SdReadFile` and `SdEntry`. Use `SdInsert` with a folder, and the folder's files
  directly once the firmware is halted.
- FatFs itself (`ff.c` and `src/sys/ff_host.c`), its disk-driver layer (`diskio.c`,
  `ff_gen_drv.c`, `option/ccsbcs.c`) and the block device behind it. libDaisy's `ff.h` still
  supplies the FatFs types.

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

[Unreleased]: https://github.com/pablo-penovi/daisycola/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/pablo-penovi/daisycola/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/pablo-penovi/daisycola/tree/v0.1.0
