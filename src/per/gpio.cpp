#include "per/gpio.h"

#include "board/board.h"

using namespace daisy;
using daisycola::board::PinMode;
using daisycola::board::PinPull;

namespace
{
PinMode ModeOf(GPIO::Mode m)
{
    return m == GPIO::Mode::OUTPUT || m == GPIO::Mode::OUTPUT_OD ? PinMode::kOutput
                                                                  : PinMode::kInput;
}

PinPull PullOf(GPIO::Pull p)
{
    switch(p)
    {
        case GPIO::Pull::PULLUP: return PinPull::kUp;
        case GPIO::Pull::PULLDOWN: return PinPull::kDown;
        default: return PinPull::kNone;
    }
}
} // namespace

void GPIO::Init(const Config& cfg)
{
    cfg_ = cfg;
    daisycola::board::ConfigurePin(cfg_.pin, ModeOf(cfg_.mode), PullOf(cfg_.pull));
}

void GPIO::Init(Pin p, const Config& cfg)
{
    Config c = cfg;
    c.pin    = p;
    Init(c);
}

void GPIO::Init(Pin p, Mode m, Pull pu, Speed sp)
{
    Config c;
    c.pin   = p;
    c.mode  = m;
    c.pull  = pu;
    c.speed = sp;
    Init(c);
}

bool GPIO::Read()
{
    return daisycola::board::ReadPin(cfg_.pin);
}

void GPIO::Write(bool state)
{
    daisycola::board::WritePin(cfg_.pin, state);
}

namespace
{
Pin ToPin(const dsy_gpio_pin& p)
{
    return Pin(GPIOPort(p.port), p.pin);
}
} // namespace

extern "C"
{
    void dsy_gpio_init(const dsy_gpio* p)
    {
        const PinMode mode = p->mode == DSY_GPIO_MODE_OUTPUT_PP || p->mode == DSY_GPIO_MODE_OUTPUT_OD
                                 ? PinMode::kOutput
                                 : PinMode::kInput;
        const PinPull pull = p->pull == DSY_GPIO_PULLUP     ? PinPull::kUp
                             : p->pull == DSY_GPIO_PULLDOWN ? PinPull::kDown
                                                            : PinPull::kNone;
        daisycola::board::ConfigurePin(ToPin(p->pin), mode, pull);
    }

    uint8_t dsy_gpio_read(const dsy_gpio* p)
    {
        return daisycola::board::ReadPin(ToPin(p->pin));
    }

    void dsy_gpio_write(const dsy_gpio* p, uint8_t state)
    {
        daisycola::board::WritePin(ToPin(p->pin), state > 0);
    }
}
