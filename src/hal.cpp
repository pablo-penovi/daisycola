// CMSIS and HAL calls, on the virtual MCU.
#include "stm32h7xx_hal.h"

#include "mcu/vmcu.h"
#include "stub.h"

extern "C"
{
    void daisycola_disable_irq(void)
    {
        daisycola::mcu::DisableIrq();
    }

    void daisycola_enable_irq(void)
    {
        daisycola::mcu::EnableIrq();
    }

    uint32_t daisycola_get_primask(void)
    {
        return daisycola::mcu::GetPrimask();
    }

    void daisycola_set_primask(uint32_t primask)
    {
        daisycola::mcu::SetPrimask(primask);
    }

    void HAL_NVIC_EnableIRQ(IRQn_Type IRQn)
    {
        daisycola::mcu::NvicSetEnabled(IRQn, true);
    }

    void HAL_NVIC_DisableIRQ(IRQn_Type IRQn)
    {
        daisycola::mcu::NvicSetEnabled(IRQn, false);
    }

    void HAL_PWR_EnterSTOPMode(uint32_t Regulator, uint8_t STOPEntry)
    {
        DAISYCOLA_STUB();
    }
}
