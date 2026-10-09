#include "sys/system.h"

#include <atomic>

#include "mcu/vmcu.h"

using namespace daisy;

namespace
{
// DaisySeed::Init(true) boosts the core to 480 MHz; the default is 400 MHz. HCLK and both APB
// clocks are divided by 2 from there, as in libDaisy's ConfigureClocks.
std::atomic<bool> boosted{false};

uint32_t SysClk()
{
    return boosted.load() ? 480000000u : 400000000u;
}

uint64_t ElapsedNs()
{
    return daisycola::mcu::NowNs();
}
} // namespace

void System::Init()
{
    Config cfg;
    cfg.Defaults();
    Init(cfg);
}

void System::Init(const Config& config)
{
    cfg_ = config;
    boosted.store(config.cpu_freq == Config::SysClkFreq::FREQ_480MHZ);
}

uint32_t System::GetNow()
{
    return uint32_t(ElapsedNs() / 1000000u);
}

// TIM2 counts at the timer clock and wraps at 32 bits, as on the device.
uint32_t System::GetTick()
{
    return uint32_t(ElapsedNs() * (GetTickFreq() / 1000000u) / 1000u);
}

uint32_t System::GetUs()
{
    return GetTick() / (GetTickFreq() / 1000000u);
}

// HAL_Delay waits for delay_ms + 1 tick boundaries, so the wait is between delay_ms and
// delay_ms + 1 milliseconds.
void System::Delay(uint32_t delay_ms)
{
    const uint64_t start_ms = ElapsedNs() / 1000000u;
    daisycola::mcu::SleepUntil((start_ms + delay_ms + 1) * 1000000u);
}

void System::DelayUs(uint32_t delay_us)
{
    daisycola::mcu::SleepUntil(ElapsedNs() + uint64_t(delay_us) * 1000u);
}

// A few ticks are a few nanoseconds: shorter than any host delay. ShiftRegister4021 calls this
// between pin writes, which the pin model doesn't need.
void System::DelayTicks(uint32_t delay_ticks)
{
    if(delay_ticks >= GetTickFreq() / 1000000u)
        daisycola::mcu::SleepUntil(ElapsedNs() + uint64_t(delay_ticks) * 1000000000u / GetTickFreq());
}

uint32_t System::GetSysClkFreq()
{
    return SysClk();
}

uint32_t System::GetHClkFreq()
{
    return SysClk() / 2;
}

uint32_t System::GetPClk1Freq()
{
    return SysClk() / 4;
}

uint32_t System::GetPClk2Freq()
{
    return SysClk() / 4;
}

uint32_t System::GetTickFreq()
{
    return GetPClk1Freq() * 2;
}
