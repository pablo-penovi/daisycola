// Timer state shared between TimerHandle and TimChannel.
#pragma once

#include <cstdint>

#include "per/tim.h"

namespace daisycola::tim
{
/** The registers of one of TIM2..TIM5 that the models need. */
struct Registers
{
    uint32_t psc; // prescaler: the counter ticks at timer clock / (psc + 1)
    uint32_t arr; // auto-reload: a period lasts arr + 1 counter ticks
};

/** Current registers of a timer. */
Registers Get(daisy::TimerHandle::Config::Peripheral periph);

/** Length of one timer period in nanoseconds, from the registers. */
uint64_t PeriodNs(const Registers& regs);

} // namespace daisycola::tim
