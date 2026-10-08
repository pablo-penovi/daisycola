# daisycola

**Daisy Compatibility Layer**: a host replacement for the hardware layer of
[libDaisy](https://github.com/electro-smith/libDaisy), so that firmware written for the
Electro-Smith Daisy platform can be compiled and run as a normal program on a Linux PC.

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

Early: nothing is implemented yet.

## Forking and contributing

You're welcome to fork daisycola and expand it for your own Daisy firmware. Covering more of
libDaisy, upstream compatibility, other boards (Pod, Patch, Field and so on) or other platforms
are all fair game. The code is MIT-licensed so that this is easy. Since the scope here stays
limited to CHOMPI, larger extensions may fit better in a fork than in this repo.

## Licence

MIT, see [LICENSE](LICENSE). libDaisy is MIT-licensed by Electro-Smith. daisycola isn't affiliated
with or endorsed by Electro-Smith or CHOMPI Club.
