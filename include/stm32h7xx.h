/* daisycola replacement for the STM32H7 CMSIS device header.
 *
 * The libDaisy Makefile force-includes stm32h7xx.h into every file, so firmware can call HAL
 * functions without including anything. daisycola's CMake target does the same with this file,
 * which declares only the parts of the HAL that CHOMPI firmware uses.
 */
#ifndef __STM32H7xx_H
#define __STM32H7xx_H

#include <stdint.h>
#include "cmsis_gcc.h"

/* Interrupt numbers of the STM32H750, for the lines the virtual MCU models. */
typedef enum
{
    DMA1_Stream0_IRQn = 11,
    DMA1_Stream1_IRQn = 12,
    DMA1_Stream3_IRQn = 14,
    DMA1_Stream4_IRQn = 15,
    DMA1_Stream6_IRQn = 17,
    TIM2_IRQn         = 28,
    TIM3_IRQn         = 29,
    TIM4_IRQn         = 30,
    I2C1_EV_IRQn      = 31,
    USART1_IRQn       = 37,
    TIM5_IRQn         = 50,
    DMA2_Stream5_IRQn = 68,
    OTG_FS_IRQn       = 101,
} IRQn_Type;

#include "stm32h7xx_hal.h"

#endif
