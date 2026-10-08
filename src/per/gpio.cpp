#include "per/gpio.h"
#include "stub.h"

using namespace daisy;

void GPIO::Init(Pin p, Mode m, Pull pu, Speed sp) { DAISYCOLA_STUB(); }
bool GPIO::Read() { DAISYCOLA_STUB(); }
void GPIO::Write(bool state) { DAISYCOLA_STUB(); }

extern "C"
{
    void dsy_gpio_init(const dsy_gpio* p) { DAISYCOLA_STUB(); }
    uint8_t dsy_gpio_read(const dsy_gpio* p) { DAISYCOLA_STUB(); }
    void dsy_gpio_write(const dsy_gpio* p, uint8_t state) { DAISYCOLA_STUB(); }
}
