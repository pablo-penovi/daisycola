/* daisycola replacement for libDaisy's daisy_seed.h.
 *
 * The DaisySeed class keeps the CHOMPI fork's declarations for the members CHOMPI firmware uses.
 * The QSPI, ADC, DAC, SDRAM, USB and LED members are left out, so firmware that touches them
 * fails to compile. The seed::D0..D32 pin map is the original.
 */
#ifndef DSY_SEED_H
#define DSY_SEED_H

#include <stdio.h>
#include "daisy.h"

namespace daisy
{
class DaisySeed
{
  public:
    DaisySeed() {}
    ~DaisySeed() {}

    /** Sets up the virtual board's clocks and the on-board codec's SAI (SAI1). */
    void Init(bool boost = false);

    /** Starts the audio interrupt with the given callback. */
    void StartAudio(AudioHandle::AudioCallback cb);

    /** Returns the audio sample rate in Hz. */
    float AudioSampleRate();

    /** Returns the number of frames per audio block. */
    size_t AudioBlockSize();

    /** Returns the SAI handle of the on-board codec. */
    const SaiHandle& AudioSaiHandle() const;

    /** Prints a formatted message to the host's standard error. */
    template <typename... VA>
    static void Print(const char* format, VA... va)
    {
        fprintf(stderr, format, va...);
    }

    /** Prints a formatted message and a newline to the host's standard error. */
    template <typename... VA>
    static void PrintLine(const char* format, VA... va)
    {
        fprintf(stderr, format, va...);
        fputc('\n', stderr);
    }

    AudioHandle audio_handle; /**< & */

  private:
    SaiHandle sai_1_handle_;
};

/** seed namespace contains pinout constants for addressing
 * the pins on the Daisy Seed SOM.
 */
namespace seed
{
    /** Constant Pinout consts */
    constexpr Pin D0  = Pin(PORTB, 12);
    constexpr Pin D1  = Pin(PORTC, 11);
    constexpr Pin D2  = Pin(PORTC, 10);
    constexpr Pin D3  = Pin(PORTC, 9);
    constexpr Pin D4  = Pin(PORTC, 8);
    constexpr Pin D5  = Pin(PORTD, 2);
    constexpr Pin D6  = Pin(PORTC, 12);
    constexpr Pin D7  = Pin(PORTG, 10);
    constexpr Pin D8  = Pin(PORTG, 11);
    constexpr Pin D9  = Pin(PORTB, 4);
    constexpr Pin D10 = Pin(PORTB, 5);
    constexpr Pin D11 = Pin(PORTB, 8);
    constexpr Pin D12 = Pin(PORTB, 9);
    constexpr Pin D13 = Pin(PORTB, 6);
    constexpr Pin D14 = Pin(PORTB, 7);
    constexpr Pin D15 = Pin(PORTC, 0);
    constexpr Pin D16 = Pin(PORTA, 3);
    constexpr Pin D17 = Pin(PORTB, 1);
    constexpr Pin D18 = Pin(PORTA, 7);
    constexpr Pin D19 = Pin(PORTA, 6);
    constexpr Pin D20 = Pin(PORTC, 1);
    constexpr Pin D21 = Pin(PORTC, 4);
    constexpr Pin D22 = Pin(PORTA, 5);
    constexpr Pin D23 = Pin(PORTA, 4);
    constexpr Pin D24 = Pin(PORTA, 1);
    constexpr Pin D25 = Pin(PORTA, 0);
    constexpr Pin D26 = Pin(PORTD, 11);
    constexpr Pin D27 = Pin(PORTG, 9);
    constexpr Pin D28 = Pin(PORTA, 2);
    constexpr Pin D29 = Pin(PORTB, 14);
    constexpr Pin D30 = Pin(PORTB, 15);

    /** Analog pins share same pins as digital pins */
    constexpr Pin A0  = D15;
    constexpr Pin A1  = D16;
    constexpr Pin A2  = D17;
    constexpr Pin A3  = D18;
    constexpr Pin A4  = D19;
    constexpr Pin A5  = D20;
    constexpr Pin A6  = D21;
    constexpr Pin A7  = D22;
    constexpr Pin A8  = D23;
    constexpr Pin A9  = D24;
    constexpr Pin A10 = D25;
    constexpr Pin A11 = D28;

    /** Pins unique to Daisy Seed 2 DFM */
    constexpr Pin D31 = Pin(PORTC, 2);
    constexpr Pin D32 = Pin(PORTC, 3);

    /** Analog Pin alias */
    constexpr Pin A12 = D31;
    constexpr Pin A13 = D32;
} // namespace seed

} // namespace daisy

#endif
