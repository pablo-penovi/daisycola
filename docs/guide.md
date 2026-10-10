# Using daisycola

How to run Daisy firmware on daisycola, and what to do when the firmware needs something daisycola
doesn't model yet. The CHOMPI TAPE tests (`tests/tape/`) are a complete working example.

## Building firmware against daisycola

The firmware's sources are compiled unchanged for the host. libDaisy's hardware sources aren't
compiled at all: daisycola provides those bodies and compiles the libDaisy sources that don't touch
hardware.

1. Point daisycola at the libDaisy tree the firmware bundles, and add it to the build:

   ```cmake
   set(DAISYCOLA_LIBDAISY_DIR ${FIRMWARE}/libs/libDaisy CACHE PATH "")
   add_subdirectory(daisycola)
   ```

2. Build the firmware as a library. Rename its `main` so the host program keeps its own, and link
   `daisycola`:

   ```cmake
   add_library(my_firmware STATIC ${FIRMWARE}/src/main.cpp ...)
   target_include_directories(my_firmware PUBLIC ${FIRMWARE}/src)
   target_compile_definitions(my_firmware PRIVATE main=firmware_main)
   target_link_libraries(my_firmware PUBLIC daisycola daisysp)
   ```

   Linking `daisycola` puts its headers before libDaisy's and force-includes the device header, as
   libDaisy's Makefile does. If the firmware uses DaisySP, its include directory must come after
   daisycola's (daisycola carries a small DaisySP fix, see [headers.md](headers.md)).

3. Build the host program and link the firmware library.

That links the firmware into the program, which runs it once per process. To power-cycle it, build
it as a firmware library instead (see [Power cycles](#power-cycles)).

If something doesn't compile or link, see below.

## A host program

`daisycola/host.h` is the whole host API. A host sets the board up, starts the firmware, then talks
to it from its own threads:

```cpp
#include "daisy_seed.h"       // seed:: pin names
#include "daisycola/host.h"

int firmware_main();          // the firmware's renamed main

int main()
{
    using namespace daisycola;
    using daisy::seed::D7, daisy::seed::D8, daisy::seed::D9;

    // 1. Wire the board.
    SdInsert("card");                             // a folder: the SD card's files
    static Mp2722 charger;                        // an I2CDevice
    AttachI2CDevice(0, 0x3f, &charger);
    const int keys = AttachSr4021(D8, D7, D9, 5); // five CD4021s on clock, latch, data
    SetAudioClock(AudioClock::kHost);             // the host's audio thread drives audio
    SetUsbConnected(true);

    // 2. Start the firmware.
    Start(firmware_main);

    // 3. Talk to it. For example, from the UI thread:
    SetSrInput(keys, 15, false);                  // press the key on input 15
    static DmaFrame frame;
    Rgb leds[25];
    if(GetDmaFrame(3, 2, frame))
        Ws2812Decode(frame, ColorOrder::kGrb, leds, 25);
    // ... and from the audio thread: ProcessAudio(in, out, frames);
    // ... and from the MIDI thread: WriteMidiIn / ReadMidiOut.

    // 4. Stop it before exiting, or before using the card folder's files.
    Halt();
}
```

Things to know:

- **Wiring comes first.** CD4021 chains and encoders can only be attached before `Start`.
- **Linked-in firmware runs once per process.** A halted firmware can't be restarted. Tests that run
  linked-in firmware need a process each (ctest runs `gtest_discover_tests` tests that way). A
  firmware library can be power-cycled instead.
- **I2C device models run on the firmware thread**, sometimes inside an interrupt. They must not
  lock, allocate or block.
- **Audio.** With the default internal clock a timer runs the audio interrupt at the sample rate,
  and the host may `WriteAudio`/`ReadAudio`. With `AudioClock::kHost`, call `ProcessAudio` from the
  audio thread; output lags input by two blocks. The firmware then reads its controls in bursts,
  one per host period: give key chains `hold_reads` (`AttachSr4021`'s last argument) so short
  taps still get through its debounce.
- **Headless runs** don't need any of the host threads: with the internal audio clock the firmware
  runs on its own. `GetBoardState`, `GetIrqStats` and `GetAudioStats` show what it is doing.
- **Firmware that uses raw SDRAM addresses** under AddressSanitizer needs
  `ASAN_OPTIONS=protect_shadow_gap=0` (see [design-notes.md](design-notes.md)).

## The SD card

The card is a folder on the host. `SdInsert(dir)` puts it in; the firmware's FatFs calls then act
on the files in it, and the host can use them with its own file tools whenever the firmware is
halted. daisycola implements FatFs's whole public API on the folder (`src/sys/ff_folder.cpp`),
following FatFs's behaviour where a folder can show it:

- names are matched without regard to case, as on FAT; new files get the name the firmware used;
- `f_rename` never replaces an existing file, and a seek past the end of a file open for writing
  extends it;
- `..` can't leave the card's folder, and only drive `0:` exists;
- `f_stat` and `f_readdir` report the size, `AM_DIR`, `AM_RDO` (no write permission) and `AM_HID`
  (names starting with a dot), with zero timestamps;
- `f_getfree` reports the host file system's free space in 32 KB clusters, `f_getlabel` and
  `f_setlabel` use the folder's name, and `f_mkfs` empties the folder;
- `f_chmod` and `f_utime` succeed and change nothing; `f_fdisk` and `f_forward` return
  `FR_INT_ERR` and say so on stderr;
- `disk_status(0)` reports whether the card is there. No other disk-layer function exists.

A folder doesn't model FAT itself. There is no allocation, no cluster size beyond what `f_getfree`
reports and no write left half done when the board is switched off: every write has either
happened or not. Firmware whose behaviour depends on those can't be tested on daisycola.

`SdSetPresent(false)` simulates pulling the card out without ejecting it: every FatFs call returns
`FR_NOT_READY` until it goes back in. Deleting the folder while the firmware runs has the same
effect, and daisycola never recreates it.

The FatFs functions run on the firmware's thread, sometimes in an interrupt (TAPE streams samples
from its timer callback), so they only use async-signal-safe system calls and fixed buffers. At
most 64 files and 16 directories can be open at once.

## Power cycles

`PowerCycle()` switches the board off and on: the firmware starts again from a clean state, while
the host's window, audio client and its own objects carry on. That needs a fresh copy of
everything the firmware keeps in globals (its own, libDaisy's and daisycola's board), so the
firmware is built as a shared library and loaded:

```cmake
daisycola_firmware_library(daisysp_fw)             # a static library that goes into it
daisycola_add_firmware(my_firmware SOURCES ${FIRMWARE}/src/main.cpp ...)
target_include_directories(my_firmware PRIVATE ${FIRMWARE}/src)
target_link_libraries(my_firmware PRIVATE daisysp_fw)

target_link_libraries(my_host PRIVATE daisycola_host)  # not daisycola
```

`daisycola_add_firmware` builds `lib<name>.so` from the firmware, the libDaisy sources and a copy
of daisycola, with `main` renamed for daisycola. Everything in it is position-independent with
hidden symbols, and compiled with `-fno-gnu-unique`, since GNU unique symbols (GCC's choice for
static locals in inline functions) would keep the library loaded after `dlclose`.
`daisycola_firmware_library` gives a static library the same flags. The library exports one
function, which hands `daisycola_host` a table of the host API. Host code calls `host.h` as
before:

```cpp
LoadFirmware("libmy_firmware.so");
Wire();                     // AttachSr4021, AttachI2CDevice, SdInsert, SetAudioClock ...
Start();                    // no argument: the library's main

// later, from the same thread:
PowerCycle();               // throws FirmwareError if it fails
Wire();                     // the board is bare again
Start();
```

A power cycle halts the firmware, releases what the library holds in the process (its POSIX
timers, signal handlers, the SDRAM mapping, the card's open files, the firmware thread), unloads
it, checks with `dlopen(RTLD_NOLOAD)` that it really is gone, and loads it again. If the firmware
doesn't halt in time, `PowerCycle` throws and the firmware keeps running. If the library can't be
unloaded, it throws too and refuses to load it again: firmware never runs with stale state.

Rules for the host:

- **No pointers into the library across a cycle.** Nothing the firmware handed out (function
  pointers, addresses of its objects) may be kept. The host's own objects that the board points
  to, such as `I2CDevice` models, survive: attach them again after the cycle.
- **Wire again after every cycle.** The board, the card and the audio clock belong to the library
  and start bare.
- **Other host threads may keep calling.** During the cycle, and whenever no library is loaded,
  `ProcessAudio` produces silence, MIDI moves no bytes and queries return zeros. Wiring calls,
  `SdInsert` and `Start` abort without a library.
- **Load, unload and power-cycle from one thread.**

## When the firmware doesn't build or link

daisycola only implements what CHOMPI firmware uses, and a missing piece fails loudly:

| Symptom | Cause |
|---|---|
| A libDaisy class or function isn't declared | It lives in a header daisycola replaces with a subset, usually `daisy.h` or `daisy_seed.h`. |
| `undefined reference` to a libDaisy function | Its header is the fork's, but daisycola hasn't written the body. |
| `daisycola: ... is not modelled` at run time | The function exists but only for the cases CHOMPI uses (for example, only USART1). |

In each case, add the missing part as described next.

## Adding a missing peripheral

[headers.md](headers.md) sorts libDaisy's headers into three kinds: replaced by daisycola, the
fork's header with daisycola's bodies, and the fork's header and source as they are. Start there.

1. **Keep the fork's header if you can.** If it only declares things, leave it alone and write the
   bodies in `src/`, at the same path as libDaisy's source (`per/spi.h` → `src/per/spi.cpp`).
   Define only the functions the firmware calls, so that anything else still fails to link. Replace
   a header only if it pulls in the STM32 HAL or does hardware work in inline code, and then match
   its declarations exactly.

2. **Model the behaviour, not the registers.** Bodies act on the virtual board:
   - pins through `board::ConfigurePin`, `ReadPin` and `WritePin` (`src/board/board.h`); chips
     wired to pins, like the CD4021 model, see the edges;
   - time through `mcu::NowNs` and `mcu::SleepUntil` (`src/mcu/vmcu.h`). A blocking transfer
     should take its time on the bus, as `I2CHandle::TransmitBlocking` does.

3. **Interrupts.** If the peripheral finishes work in an interrupt (DMA complete, receive):
   - add a line to `mcu::Line` in priority order (priority-0 sources before the timers) and give
     it the same place in `Irq` in `host.h`;
   - add its `IRQn_Type` to `include/stm32h7xx.h` and register it with `mcu::SetIrqNumber`, so
     `HAL_NVIC_DisableIRQ` works on it;
   - set the handler with `mcu::SetHandler`; `mcu::Schedule(line, transfer_ns)` fires it when a
     DMA transfer would be done, `mcu::Raise(line)` when the host delivers something.

   Handlers run in a signal handler on the firmware thread: no locks, no allocation, nothing that
   isn't async-signal-safe. Raises can merge, so a handler should process everything that is
   waiting, not one item per run.

4. **Cross to the host without locks.** The firmware side never waits for the host. Share state
   through atomics or `SpscRing` (`src/util/spsc_ring.h`). When the firmware writes from both main
   and interrupt context, wrap the write in `mcu::Critical` so the two can't interleave. Host-side
   functions may take a mutex among host threads (see `src/board/midi.cpp`). Add the host
   functions to `host.h`.

5. **Test it.** Unit tests can drive the peripheral single-threaded with the manual clock
   (`tests/unit/`). If interrupts are involved, add a small firmware to `tests/mcu/`. Run the
   suite in a ThreadSanitizer and an AddressSanitizer build too.

6. **Write it down.** Add the header to [headers.md](headers.md), with how the fork differs from
   upstream, and add a note to [design-notes.md](design-notes.md) if the model makes a choice that
   isn't obvious from the code.

The UART is a compact example of all of this: `src/per/uart.cpp` defines four `UartHandler`
functions on the fork's header, `src/board/midi.cpp` holds the rings and the host functions, and
`tests/mcu/firmware_test.cpp` checks it through TAPE's own `MidiHandler`.
