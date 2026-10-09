#include "mcu/vmcu.h"

#include <atomic>
#include <ctime>

namespace daisycola::mcu
{
namespace
{
struct LineState
{
    std::atomic<Handler>   handler{nullptr};
    std::atomic<void*>     context{nullptr};
    std::atomic<int>       irqn{-1};
    std::atomic<int>       pending{0};
    std::atomic<uint64_t>  due_ns{0}; // one-shot; 0 = none
    std::atomic<uint64_t>  period_ns{0};
    std::atomic<uint64_t>  next_ns{0};
    std::atomic<uint64_t>  count{0};
    std::atomic<uint64_t>  total_ns{0};
    std::atomic<uint64_t>  max_ns{0};
    std::atomic<uint64_t>  dropped{0};
};

LineState lines[kLineCount];

// CPU state. Only the thread acting as the MCU touches it.
std::atomic<uint32_t> primask{0};
std::atomic<uint32_t> nvic_disabled{0}; // bit per line
std::atomic<bool>     all_masked{false};
std::atomic<int>      depth{0};

std::atomic<bool>     manual_clock{false};
std::atomic<uint64_t> manual_now{0};

uint64_t MonotonicNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// The board "boots" the first time anything reads the clock.
uint64_t Epoch()
{
    static const uint64_t epoch = MonotonicNs();
    return epoch;
}

bool Blocked(int i)
{
    return primask.load() || all_masked.load() || (nvic_disabled.load() >> i & 1);
}

bool Ready(int i, uint64_t now)
{
    LineState& l = lines[i];
    if(l.pending.load() > 0)
        return true;
    const uint64_t due = l.due_ns.load();
    if(due != 0 && now >= due)
        return true;
    const uint64_t period = l.period_ns.load();
    return period != 0 && now >= l.next_ns.load();
}

void Consume(int i, uint64_t now)
{
    LineState& l = lines[i];
    if(l.pending.load() > 0)
    {
        l.pending.fetch_sub(1);
        return;
    }
    const uint64_t due = l.due_ns.load();
    if(due != 0 && now >= due)
    {
        l.due_ns.store(0);
        return;
    }
    // A timer whose handler fell behind fires once, then waits for its next period boundary.
    const uint64_t period = l.period_ns.load();
    uint64_t       next   = l.next_ns.load() + period;
    if(next <= now)
        next += (now - next) / period * period + period;
    l.next_ns.store(next);
}

void RunHandler(int i)
{
    LineState&     l       = lines[i];
    const Handler  handler = l.handler.load();
    const uint32_t saved   = primask.exchange(0);
    depth.fetch_add(1);
    const uint64_t t0 = MonotonicNs();
    if(handler)
        handler(l.context.load());
    const uint64_t took = MonotonicNs() - t0;
    depth.fetch_sub(1);
    primask.store(saved);
    l.count.fetch_add(1);
    l.total_ns.fetch_add(took);
    if(took > l.max_ns.load())
        l.max_ns.store(took);
}

} // namespace

void SetHandler(Line line, Handler handler, void* context)
{
    lines[int(line)].context.store(context);
    lines[int(line)].handler.store(handler);
}

void SetIrqNumber(Line line, IRQn_Type irqn)
{
    lines[int(line)].irqn.store(int(irqn));
}

void Raise(Line line)
{
    lines[int(line)].pending.fetch_add(1);
}

void Schedule(Line line, uint64_t delay_ns)
{
    // At least 1 ns so that a zero delay still reads as "scheduled".
    lines[int(line)].due_ns.store(NowNs() + (delay_ns ? delay_ns : 1));
}

void SetPeriodic(Line line, uint64_t period_ns)
{
    LineState& l = lines[int(line)];
    l.next_ns.store(NowNs() + period_ns);
    l.period_ns.store(period_ns);
}

bool InInterrupt()
{
    return depth.load() > 0;
}

void DisableIrq()
{
    primask.store(1);
}

void EnableIrq()
{
    primask.store(0);
}

uint32_t GetPrimask()
{
    return primask.load();
}

void SetPrimask(uint32_t value)
{
    primask.store(value & 1);
}

void NvicSetEnabled(IRQn_Type irqn, bool enabled)
{
    for(int i = 0; i < kLineCount; i++)
    {
        if(lines[i].irqn.load() != int(irqn))
            continue;
        if(enabled)
            nvic_disabled.fetch_and(~(1u << i));
        else
            nvic_disabled.fetch_or(1u << i);
    }
}

void SetAllMasked(bool masked)
{
    all_masked.store(masked);
}

uint64_t NowNs()
{
    if(manual_clock.load())
        return manual_now.load();
    const uint64_t epoch = Epoch(); // first, so the first call reads 0 rather than wrapping
    return MonotonicNs() - epoch;
}

size_t ServiceInterrupts()
{
    size_t ran = 0;
    for(;;)
    {
        const uint64_t now   = NowNs();
        int            found = -1;
        for(int i = 0; i < kLineCount && found < 0; i++)
            if(!Blocked(i) && Ready(i, now))
                found = i;
        if(found < 0)
            return ran;
        Consume(found, now);
        RunHandler(found);
        ran++;
    }
}

void SleepUntil(uint64_t t_ns)
{
    // Inside a handler only higher-priority interrupts could run, and the single-threaded mode
    // has no notion of nesting: just let the time pass.
    if(InInterrupt())
    {
        if(manual_clock.load())
        {
            if(t_ns > manual_now.load())
                manual_now.store(t_ns);
            return;
        }
        for(uint64_t now = NowNs(); now < t_ns; now = NowNs())
        {
            const uint64_t left = t_ns - now;
            const timespec ts{time_t(left / 1000000000ull), long(left % 1000000000ull)};
            nanosleep(&ts, nullptr);
        }
        return;
    }

    if(!manual_clock.load())
    {
        for(uint64_t now = NowNs(); now < t_ns; now = NowNs())
        {
            ServiceInterrupts();
            const uint64_t  left = t_ns - now;
            const timespec  ts{time_t(left / 1000000000ull), long(left % 1000000000ull)};
            nanosleep(&ts, nullptr);
        }
        ServiceInterrupts();
        return;
    }

    // Manual clock: step from one due interrupt to the next, like a discrete-event simulator.
    for(;;)
    {
        ServiceInterrupts();
        uint64_t next = t_ns;
        for(int i = 0; i < kLineCount; i++)
        {
            const uint64_t due = lines[i].due_ns.load();
            if(due != 0 && due < next)
                next = due;
            if(lines[i].period_ns.load() != 0 && lines[i].next_ns.load() < next)
                next = lines[i].next_ns.load();
        }
        if(next <= manual_now.load())
        {
            if(manual_now.load() >= t_ns)
                return;
            next = t_ns; // the due interrupt is masked; skip ahead
        }
        manual_now.store(next);
    }
}

void UseManualClock(bool manual)
{
    if(manual)
        manual_now.store(NowNs());
    else
        manual_now.store(0);
    manual_clock.store(manual);
}

void AdvanceClock(uint64_t ns)
{
    SleepUntil(NowNs() + ns);
}

bool FirmwareRunning()
{
    return false;
}

LineStats GetLineStats(Line line)
{
    const LineState& l = lines[int(line)];
    return {l.count.load(), l.total_ns.load(), l.max_ns.load(), l.dropped.load()};
}

} // namespace daisycola::mcu
