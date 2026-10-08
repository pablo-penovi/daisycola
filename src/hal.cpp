#include "stm32h7xx_hal.h"
#include "stub.h"

extern "C"
{
    void     daisycola_disable_irq(void) { DAISYCOLA_STUB(); }
    void     daisycola_enable_irq(void) { DAISYCOLA_STUB(); }
    uint32_t daisycola_get_primask(void) { DAISYCOLA_STUB(); }
    void     daisycola_set_primask(uint32_t primask) { DAISYCOLA_STUB(); }

    void HAL_NVIC_EnableIRQ(IRQn_Type IRQn) { DAISYCOLA_STUB(); }
    void HAL_NVIC_DisableIRQ(IRQn_Type IRQn) { DAISYCOLA_STUB(); }
    void HAL_PWR_EnterSTOPMode(uint32_t Regulator, uint8_t STOPEntry) { DAISYCOLA_STUB(); }
}
