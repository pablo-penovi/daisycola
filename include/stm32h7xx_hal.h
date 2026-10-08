/* daisycola replacement for the STM32H7 HAL header: only what CHOMPI firmware calls. */
#ifndef __STM32H7xx_HAL_H
#define __STM32H7xx_HAL_H

#include "stm32h7xx.h"

#define PWR_MAINREGULATOR_ON ((uint32_t)0x00000000U)
#define PWR_LOWPOWERREGULATOR_ON ((uint32_t)0x00000001U)
#define PWR_STOPENTRY_WFI ((uint8_t)0x01U)
#define PWR_STOPENTRY_WFE ((uint8_t)0x02U)

#ifdef __cplusplus
extern "C"
{
#endif

    /** Masks or unmasks one interrupt line of the virtual MCU. */
    void HAL_NVIC_EnableIRQ(IRQn_Type IRQn);
    void HAL_NVIC_DisableIRQ(IRQn_Type IRQn);

    /** Tells the host the device is asleep and blocks until the host wakes it. */
    void HAL_PWR_EnterSTOPMode(uint32_t Regulator, uint8_t STOPEntry);

#ifdef __cplusplus
}
#endif

#endif
