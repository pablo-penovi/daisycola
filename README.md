# The Daisy Compatibility Layer (daisycola)

A host replacement for the hardware layer of [libDaisy](https://github.com/electro-smith/libDaisy), 
so that firmware written for the Electro-Smith Daisy platform can be compiled and run as a normal 
program on a Linux PC.

## FULL DISCLOSURE

This is a fully vibe coded app. My goal was to get a functional, complete, native Linux virtual [CHOMPI](https://github.com/CHOMPI-Club/CHOMPI),
not the most efficient, good or secure version of it. I have not reviewed the code. This is provided
as-is and I make no promises concerning quality or security.

## What it is

libDaisy is the hardware library for Daisy boards (STM32H750). Firmware calls it for GPIO,
timers, audio (SAI), UART/USB MIDI, the SD card, shift registers and so on. daisycola provides
headers with the same names and interfaces, backed by software instead of registers. The firmware
sources are compiled unchanged for x86. A program that hosts them supplies the audio, MIDI,
controls and LEDs.

It works at the source level. It doesn't emulate the ARM CPU (that's what Renode or QEMU would
do). Parts of libDaisy that don't touch hardware, such as its UI helpers, MIDI parser and FIFOs,
and DaisySP, already build on the host and are used as they are.

## Why it exists

It's being built for [CHAMPI](https://github.com/pablo-penovi/CHAMPI), a Linux virtual instrument
that runs the real [CHOMPI](https://github.com/CHOMPI-Club/CHOMPI) TAPE firmware. As far as we
could find, no source-level libDaisy host layer existed, so we're writing one.

## Scope and limits

This is **not** a complete libDaisy compatibility layer, and it doesn't aim to be one.

- **Driven by CHOMPI.** Only the parts of libDaisy that the CHOMPI firmware uses are implemented,
  starting with TAPE and maybe later TEMPO and WAVE. Anything else may be missing, partial or a
  stub.
- **Matches CHOMPI's libDaisy fork.** CHOMPI ships a modified fork of libDaisy. Where the fork and
  upstream differ, daisycola follows the fork.
- **Linux only.** It uses POSIX threads, signals and timers. Other platforms aren't planned.
- **Behaviour, not cycle accuracy.** The goal is that firmware behaves the same, not that timing
  matches the real chip exactly.
- **No guarantees.** A passing run on the host isn't proof that firmware works on real hardware.

## Status

TAPE builds unchanged against daisycola and boots headless from its factory SD card: the firmware
runs on its own thread with prioritised interrupts, audio, MIDI, the SD card, GPIO, CD4021 shift
registers, encoders, I2C, timers and the WS2812 LED DMA. The SD card is a folder on the host, so
samples are managed with ordinary file tools. Built as a shared library, the firmware can be
power-cycled in place: it starts again from scratch while the host program keeps running. The test
suite also runs clean under ThreadSanitizer and AddressSanitizer. TEMPO and WAVE come later.

## Building

daisycola builds against a libDaisy tree, the CHOMPI fork for now. If a CHOMPI checkout sits next
to daisycola, it is found automatically and the TAPE tests are built too:

```sh
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build
```

Otherwise point CMake at the trees with `-DDAISYCOLA_CHOMPI_DIR=...` or
`-DDAISYCOLA_LIBDAISY_DIR=...`. Add `-DDAISYCOLA_SANITIZER=thread` or `address` for a sanitizer
build.

[docs/guide.md](docs/guide.md) shows how to run firmware on daisycola and how to add a peripheral it
doesn't model yet. [docs/headers.md](docs/headers.md) lists which libDaisy headers daisycola
replaces, and [docs/design-notes.md](docs/design-notes.md) explains the main design decisions.

## Forking and contributing

You're welcome to fork daisycola and expand it for your own Daisy firmware. Covering more of
libDaisy, upstream compatibility, other boards (Pod, Patch, Field and so on) or other platforms
are all fair game. The code is MIT-licensed so that this is easy. Since the scope here stays
limited to CHOMPI, larger extensions may fit better in a fork than in this repo.

## Licence

MIT, see [LICENSE](LICENSE). libDaisy is MIT-licensed by Electro-Smith. daisycola isn't affiliated
with or endorsed by Electro-Smith or CHOMPI Club.
