/* daisycola replacement for CMSIS cmsis_gcc.h.
 *
 * Only the core-register intrinsics that libDaisy and CHOMPI use are provided. They act on the
 * virtual MCU: PRIMASK masks the firmware thread's interrupt signals.
 */
#ifndef __CMSIS_GCC_H
#define __CMSIS_GCC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    void     daisycola_disable_irq(void);
    void     daisycola_enable_irq(void);
    uint32_t daisycola_get_primask(void);
    void     daisycola_set_primask(uint32_t primask);

    static inline void __disable_irq(void) { daisycola_disable_irq(); }
    static inline void __enable_irq(void) { daisycola_enable_irq(); }
    static inline uint32_t __get_PRIMASK(void) { return daisycola_get_primask(); }
    static inline void __set_PRIMASK(uint32_t primask) { daisycola_set_primask(primask); }

#ifdef __cplusplus
}
#endif

#endif
