// A firmware for the power-cycle tests (power_cycle_test.cpp), built as a firmware library.
//
// Every load should start it from scratch. main counts its runs in a zero-initialised global, a
// constructor counts itself, and an inline function counts in a static local (the kind of
// variable GCC makes a GNU unique symbol, which would keep the library loaded). main sends the
// three counts to an I2C device at 0x42: all 1 after every power cycle. Audio then plays a
// constant 0.25, so the host can tell the firmware's output from the loader's silence.
#include "daisy_seed.h"

using namespace daisy;

inline int& StaticLocal()
{
    static int count = 0;
    return count;
}

namespace
{
int runs          = 0;
int constructions = 0;

struct Counted
{
    Counted() { constructions++; }
} counted;

DaisySeed hw;
I2CHandle i2c;

void Audio(AudioHandle::InputBuffer, AudioHandle::OutputBuffer out, size_t size)
{
    for(size_t i = 0; i < size; i++)
        out[0][i] = out[1][i] = 0.25f;
}
} // namespace

int main()
{
    runs++;
    StaticLocal()++;
    hw.Init();

    I2CHandle::Config cfg;
    cfg.mode           = I2CHandle::Config::Mode::I2C_MASTER;
    cfg.periph         = I2CHandle::Config::Peripheral::I2C_1;
    cfg.speed          = I2CHandle::Config::Speed::I2C_400KHZ;
    cfg.pin_config.scl = seed::D11;
    cfg.pin_config.sda = seed::D12;
    i2c.Init(cfg);
    uint8_t report[3] = {uint8_t(runs), uint8_t(constructions), uint8_t(StaticLocal())};
    i2c.TransmitBlocking(0x42, report, sizeof report, 100);

    hw.StartAudio(Audio);
    for(;;)
        System::Delay(1);
}
