#include "per/tim.h"

#include "per/tim_internal.h"
#include "mcu/vmcu.h"
#include "sys/system.h"

using namespace daisy;

class TimerHandle::Impl
{
  public:
    TimerHandle::Config   config;
    uint32_t              psc = 0;
    uint32_t              arr = 0;
    PeriodElapsedCallback callback = nullptr;
    void*                 data     = nullptr;
    bool                  running  = false;

    daisycola::mcu::Line Line() const
    {
        return daisycola::mcu::Line(int(daisycola::mcu::Line::kTim2) + int(config.periph));
    }

    // TIM3 and TIM4 are 16-bit: libDaisy truncates the period register.
    bool Is16Bit() const
    {
        return config.periph == Config::Peripheral::TIM_3
               || config.periph == Config::Peripheral::TIM_4;
    }

    void Arm()
    {
        if(running && config.enable_irq)
            daisycola::mcu::SetPeriodic(Line(), daisycola::tim::PeriodNs({psc, arr}));
    }

    static void Elapsed(void* context)
    {
        auto* self = static_cast<Impl*>(context);
        if(self->callback)
            self->callback(self->data);
    }
};

namespace
{
TimerHandle::Impl timers[4];

constexpr IRQn_Type kIrq[4] = {TIM2_IRQn, TIM3_IRQn, TIM4_IRQn, TIM5_IRQn};
} // namespace

namespace daisycola::tim
{
Registers Get(TimerHandle::Config::Peripheral periph)
{
    const TimerHandle::Impl& t = timers[int(periph)];
    return {t.psc, t.arr};
}

uint64_t PeriodNs(const Registers& regs)
{
    const uint64_t clock_hz = uint64_t(daisy::System::GetPClk1Freq()) * 2;
    return (uint64_t(regs.psc) + 1) * (uint64_t(regs.arr) + 1) * 1000000000ull / clock_hz;
}
} // namespace daisycola::tim

TimerHandle::Result TimerHandle::Init(const Config& config)
{
    const int idx = int(config.periph);
    if(idx < 0 || idx >= 4)
        return Result::ERR;
    Impl& t  = timers[idx];
    t.config = config;
    t.psc    = 0;
    t.arr    = t.Is16Bit() ? uint16_t(config.period) : config.period;
    pimpl_   = &t;
    daisycola::mcu::SetHandler(t.Line(), Impl::Elapsed, &t);
    daisycola::mcu::SetIrqNumber(t.Line(), kIrq[idx]);
    return Result::OK;
}

TimerHandle::Result TimerHandle::SetPeriod(uint32_t ticks)
{
    pimpl_->arr = ticks;
    pimpl_->Arm();
    return Result::OK;
}

TimerHandle::Result TimerHandle::SetPrescaler(uint32_t val)
{
    pimpl_->psc = val;
    pimpl_->Arm();
    return Result::OK;
}

TimerHandle::Result TimerHandle::Start()
{
    pimpl_->running = true;
    pimpl_->Arm();
    return Result::OK;
}

const TimerHandle::Config& TimerHandle::GetConfig() const
{
    return pimpl_->config;
}

void TimerHandle::SetCallback(PeriodElapsedCallback cb, void* data)
{
    pimpl_->data     = data;
    pimpl_->callback = cb;
}
