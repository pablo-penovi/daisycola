# Header map

Where each libDaisy header that CHOMPI TAPE reaches comes from in a daisycola build, and how the
CHOMPI fork differs from upstream libDaisy.

- **Fork:** `CHOMPI/firmware/chompi-tape/code/libs/libDaisy` at CHOMPI `a73d732`. TEMPO and WAVE
  bundle the same fork, except for `hid/midi.h`, `per/tim.h`, `per/tim.cpp` and `per/uart.cpp`.
- **Upstream:** [libDaisy v5.4.0](https://github.com/electro-smith/libDaisy/tree/v5.4.0), the
  newest release in the fork's `CHANGELOG.md`. The fork's `version.h` still says 4.0.0.

The build puts `include/` and `compat/daisysp/` before libDaisy's `src/`, `src/sys/` and FatFs's
`src/`, so a header daisycola provides replaces the original.

## How headers are handled

There are three kinds:

1. **Replaced.** daisycola ships a header at the same path, because the original pulls in the STM32
   HAL or does hardware work in inline code.
2. **Original header, daisycola bodies.** The original header only declares things. daisycola
   doesn't ship the header, so the firmware sees the fork's exact declarations. daisycola defines
   the functions in `src/`. Functions CHOMPI doesn't use are left undefined, so using one fails
   at link time.
3. **Original header and source.** Not hardware-specific. daisycola compiles the fork's code as it
   is.

The plan expected more of kind 1. Keeping the original header wherever possible means there is
nothing to copy and nothing that can drift from the fork. In particular `dev/sr_4021.h` is used as
it is: daisycola models the CD4021 chips at the pin level, so the fork's own `Update()` and
debounce code run unchanged.

Some libDaisy files include other libDaisy headers by relative path (`ui/UI.cpp` includes
`"../sys/system.h"`, `ui/UiEventQueue.h` includes `"../util/scopedirqblocker.h"`). Relative
includes bypass the include path, so those headers can't be replaced and must stay kind 2 or 3.

## Replaced headers (kind 1)

| daisycola header | Replaces | Why | Fork vs upstream |
|---|---|---|---|
| `include/daisy.h` | `src/daisy.h` | The original includes every driver, most of them HAL-based. This one includes only what CHOMPI uses, plus `util/wav_format.h` (TAPE gets `WAV_FormatTypeDef` through `daisy.h`). | Fork adds `per/tim_channel.h`. |
| `include/daisy_seed.h` | `src/daisy_seed.h` | `DaisySeed` has QSPI, ADC, DAC, SDRAM and USB members. daisycola keeps only `Init`, `StartAudio(AudioCallback)`, `AudioSampleRate`, `AudioBlockSize`, `AudioSaiHandle`, `audio_handle`, `Print`/`PrintLine` (to stderr) and the `seed::` pin map. | Same header. `daisy_seed.cpp` differs (bootloader version check). |
| `include/daisy_core.h` | `src/daisy_core.h` | Wrapper: `#include_next`s the original, then empties `DMA_BUFFER_MEM_SECTION` and `DTCM_MEM_SECTION`. | Fork changes `S162F_SCALE` to 1/(2^15-1) and `s162f`/`f2s16` to `int32_t`. daisycola uses the fork's as they are. |
| `include/dev/sdram.h` | `src/dev/sdram.h` | Same declarations, with `DSY_SDRAM_BSS` and `DSY_SDRAM_DATA` empty. | Same. |
| `include/daisycola/ff_integer.h` | FatFs `integer.h` | Force-included into every file. FatFs's `integer.h` makes `DWORD` an `unsigned long`, 64 bits on x86-64; FatFs needs 32. This header defines the types at their required widths and sets `integer.h`'s include guard, so every file agrees on the layout of `FIL`, `FATFS` and the rest. | `ff.c`: only braces and a debug variable differ. `sys/ffconf.h`: the fork sets `_FS_RPATH` to 2 (upstream 0). |
| `include/stm32h7xx.h`, `include/stm32h7xx_hal.h` | CMSIS device header and HAL | Force-included into every file, as libDaisy's Makefile does. Declares the `IRQn_Type` values daisycola models, `HAL_NVIC_EnableIRQ/DisableIRQ`, `HAL_PWR_EnterSTOPMode` and the `PWR_*` constants. | n/a |
| `include/cmsis_gcc.h` | CMSIS `cmsis_gcc.h` | `__disable_irq`, `__enable_irq`, `__get_PRIMASK`, `__set_PRIMASK` on the virtual MCU. This is how `util/scopedirqblocker.h` (kind 3) works on the host. | n/a |
| `include/Limiter.h` | none | TAPE includes `"Limiter.h"`; the file is `limiter.h`. Forwards the include. Needs the firmware source directory on the include path. | n/a |
| `compat/daisysp/daisysp.h`, `compat/daisysp/Utility/delayline.h` | DaisySP `daisysp.h`, `Utility/delayline.h` | Not libDaisy, but a host snag in the same spirit: TAPE calls `DelayLine::SetDelay(10u)`, which is ambiguous on x86-64 (`size_t` is not `unsigned int`). daisycola's copy of `delayline.h` adds a `SetDelay(unsigned int)` overload. | n/a |
| `include/ff.h` | FatFs `ff.h` | Wrapper: `#include_next`s the original, then, for C++ only, redefines `f_size()` to evaluate as on the STM32. Another host snag: TAPE computes sample points as `val * (f_size(fp) - sizeof(WAV_FormatTypeDef))` into a `uint32_t`, also before the file is open, when the size is 0. On the chip `size_t` is 32 bits, the product rounds to 2^32 and GCC-ARM's float-to-u32 conversion saturates to 0xFFFFFFFF, so the point is accepted. On x86-64 the subtraction is 64-bit and the conversion wraps to 0, so the point is rejected and a cubbi's first press is silent. The wrapper's `f_size()` returns a `ChipSize`: integer arithmetic on it wraps at 32 bits, and a product with a `float` or `double` converts only to `uint32_t`, with saturation. Any other use of a product fails to compile. C files keep the original macro. TAPE reaches `ff.h` through `sys/fatfs.h`, which goes through the include path. | See `ff_integer.h` above. |

## Original header, daisycola bodies (kind 2)

| Header | daisycola source | Fork vs upstream |
|---|---|---|
| `sys/system.h` | `src/sys/system.cpp` | Fork adds `BootInfo`, `BootloaderMode`, `InitBackupSram`, `GetBootloaderVersion`. |
| `per/gpio.h` (`GPIO`, `dsy_gpio_*`) | `src/per/gpio.cpp` | Same. |
| `per/tim.h` | `src/per/tim.cpp` | Fork adds `optimize("-O0")` to `Init` and `Start`. |
| `per/tim_channel.h` | `src/per/tim_channel.cpp` | Fork only. No include guard. |
| `per/i2c.h` | `src/per/i2c.cpp` | Same. |
| `per/sai.h` | `src/per/sai.cpp` | Same. daisycola keeps only the configuration; the data path is `AudioHandle`'s. |
| `hid/audio.h` | `src/hid/audio.cpp` | Same header. Fork's `audio.cpp` puts the callback on SAI2 and swaps channel pairs; daisycola presents the four channels in the order the callback sees them. |
| `per/uart.h` | `src/per/uart.cpp` | Same header. `uart.cpp` differs between TAPE and TEMPO/WAVE. daisycola models USART1 only, with what MIDI uses: `DmaListenStart`, `IsListening`, `PollTx`. |
| `hid/usb_midi.h` (`MidiUsbTransport`) | `src/hid/usb_midi.cpp` | Fork adds `Reset()`; `Tx` returns `bool`. |
| `per/sdmmc.h` | `src/per/sdmmc.cpp` | Same header. Fork's `sdmmc.cpp` changes the IRQ priority. |
| `sys/fatfs.h` | `src/sys/fatfs.cpp` | Same. daisycola's `Init` links no disk driver: it names drive `0:/` the SD card. |
| FatFs `ff.h` (the API) | `src/sys/ff_folder.cpp` | FatFs R0.12c in both. daisycola implements every public function in `ff.h` on the card folder instead of compiling `ff.c`, which needs a block device. `f_write` accepts a null byte count: CHOMPI passes `NULL` when it doesn't need it, and FatFs stores the count through it, which on the STM32 lands unnoticed in ITCM RAM at address 0 and on Linux crashes. See [guide.md](guide.md#the-sd-card) for what a folder card does and doesn't model. |
| FatFs `diskio.h` | `src/sys/ff_folder.cpp` | Only `disk_status` and `disk_initialize`, which report whether the card is there. TAPE polls `disk_status(0)`. Sector access has no meaning on a folder and isn't provided. |
| `sys/dma.h` | `src/sys/dma.cpp` | Same. Cache maintenance is a no-op. |

## Original header and source (kind 3)

| Header / source | Notes | Fork vs upstream |
|---|---|---|
| `dev/sr_4021.h` | Bit-bangs the chips over `dsy_gpio`; daisycola's pin model answers. | Fork adds `dbc_size`, debounce, `RawState`, `RisingEdge`, `FallingEdge`. |
| `hid/midi.h`, `hid/midi.cpp` | Inline handler code over `UartHandler` and `MidiUsbTransport`. | Fork adds `Reset()`, `ResetTransport()`, `bool` sends. Differs again in TEMPO/WAVE. |
| `hid/midi_parser.h/.cpp`, `hid/MidiEvent.h` | | Same. |
| `hid/switch.h/.cpp` | Reads its pin through `dsy_gpio`. | Same. |
| `hid/usb.h` | Declarations only; nothing calls them. | Fork adds `Reset()`. |
| `ui/UI.h/.cpp`, `ui/UiEventQueue.h` | | Same. |
| `util/FIFO.h` | | Fork adds `MassPushBack`, `PopFrontMany`. |
| `util/scopedirqblocker.h` | Uses daisycola's `cmsis_gcc.h`. | Same. |
| `util/wav_format.h`, `util/ringbuffer.h`, `util/Stack.h` | | Same. |

## Not provided

Everything else in libDaisy: QSPI, ADC, DAC, SPI, SDRAM driver, codecs, displays, USB host and
device classes, `Logger`, `CpuLoadMeter` and the board classes other than `DaisySeed`. Firmware
that uses them fails to compile or link against daisycola.
