#include "mcu/vmcu.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <initializer_list>
#include <pthread.h>
#include <setjmp.h>
#include <sys/prctl.h>
#include <ucontext.h>
#include <unistd.h>

#include "board/board.h"

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

constexpr uint32_t kAllLines   = (1u << kLineCount) - 1;
constexpr uint32_t kTimerLines = kAllLines & ~((1u << int(Line::kTim2)) - 1);

// NVIC priorities: every DMA, I2C, UART and USB interrupt is 0, the timers are 15. A handler
// masks the lines its priority keeps out: equal or lower priority.
uint32_t LevelMask(int line)
{
    return line >= int(Line::kTim2) ? kTimerLines : kAllLines;
}

// CPU state. Only the thread acting as the MCU touches it.
std::atomic<uint32_t> primask{0};
std::atomic<uint32_t> nvic_disabled{0}; // bit per line
std::atomic<bool>     all_masked{false};
std::atomic<int>      depth{0};
std::atomic<uint32_t> level{0}; // lines masked by the priority of the running handler

std::atomic<bool>     manual_clock{false};
std::atomic<uint64_t> manual_now{0};

// ---- Firmware thread state ----------------------------------------------------------------------

std::atomic<bool>     started{false};
std::atomic<bool>     threaded{false}; // interrupts are signals from now on (never goes back)
std::atomic<bool>     running{false};
std::atomic<bool>     exited{false};
std::atomic<int>      exit_code{0};
std::atomic<bool>     halt_requested{false};
std::atomic<bool>     sleeping{false};
std::atomic<uint64_t> sleeps{0};
std::atomic<uint64_t> wake_seq{0};
std::atomic<bool>     thread_ended{false}; // the firmware thread returned and can be joined
std::atomic<bool>     joined{false};

pthread_t              fw_thread;
std::atomic<pthread_t> fw_self{}; // set by the firmware thread itself
jmp_buf                park_jmp;  // where Park returns to, in FirmwareThread
timer_t                timers[kLineCount];
bool                   timer_created[kLineCount];
bool                   handlers_installed = false;
struct sigaction       old_actions[kLineCount]; // what InstallHandlers replaced
sigset_t               irq_signals;   // every line's signal
sigset_t               other_signals; // non-interrupt signals, blocked on the firmware thread for good

void Fail(const char* message)
{
    std::fprintf(stderr, "daisycola: %s\n", message);
    std::abort();
}

// One real-time signal per line. Linux delivers the lowest pending real-time signal first, so
// with lines numbered by priority, pending interrupts run in the NVIC's order.
int SignalOf(int line)
{
    return SIGRTMIN + 2 + line;
}

// No thread_local: a firmware library's TLS block on a host thread would outlive dlclose.
bool OnFirmwareThread()
{
    return threaded.load() && pthread_equal(pthread_self(), fw_self.load());
}

// The signal mask that matches the CPU state, for a given handler level.
sigset_t MaskFor(uint32_t level_lines)
{
    uint32_t masked = level_lines | nvic_disabled.load();
    if(primask.load() || all_masked.load())
        masked = kAllLines;
    sigset_t set = other_signals;
    for(int i = 0; i < kLineCount; i++)
        if(masked >> i & 1)
            sigaddset(&set, SignalOf(i));
    return set;
}

// Brings the firmware thread's signal mask in line with the CPU state. Blocks every line first
// so that no handler runs between reading the state and setting the mask.
void ApplyMask()
{
    if(!OnFirmwareThread())
        return;
    pthread_sigmask(SIG_BLOCK, &irq_signals, nullptr);
    const sigset_t set = MaskFor(level.load());
    pthread_sigmask(SIG_SETMASK, &set, nullptr);
}

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

timespec ToTimespec(uint64_t ns)
{
    return {time_t(ns / 1000000000ull), long(ns % 1000000000ull)};
}

// After a halt request the firmware stops here for good. It jumps back to FirmwareThread, with
// every interrupt masked, and the thread ends. Nothing unwinds: no destructor in the firmware
// runs, so nothing it was doing gets torn down under the host.
//
// _longjmp rather than siglongjmp: ThreadSanitizer (GCC 16, glibc 2.44) can't follow the
// mask-saving jumps. Blocking every signal first leaves the same state siglongjmp would.
[[noreturn]] void Park()
{
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, nullptr);
    _longjmp(park_jmp, 1);
}

// Halting waits for main context: an interrupt handler may be halfway through something (an SD
// write in TAPE's timer callback) that the host will want to see finished.
void CheckHalt()
{
    if(halt_requested.load(std::memory_order_relaxed) && OnFirmwareThread() && depth.load() == 0)
        Park();
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

// The signal handler for every line. The kernel has already masked the lines of this priority
// (sa_mask). On the way out the interrupted context's mask is recomputed rather than restored,
// in case the handler changed the NVIC enables.
void Dispatch(int sig, siginfo_t*, void* ucontext)
{
    const int      saved_errno = errno;
    const int      i           = sig - SignalOf(0);
    const uint32_t saved_level = level.exchange(LevelMask(i));
    RunHandler(i);
    level.store(saved_level);
    static_cast<ucontext_t*>(ucontext)->uc_sigmask = MaskFor(saved_level);
    errno = saved_errno;
}

void ArmTimer(int i, uint64_t value_ns, uint64_t interval_ns)
{
    itimerspec spec{ToTimespec(interval_ns), ToTimespec(value_ns)};
    timer_settime(timers[i], 0, &spec, nullptr);
}

struct Boot
{
    int (*firmware_main)();
    std::atomic<bool> ready{false};
};

void InstallHandlers()
{
    sigemptyset(&irq_signals);
    for(int i = 0; i < kLineCount; i++)
        sigaddset(&irq_signals, SignalOf(i));

    // Synchronous signals stay unblocked so that crashes still reach the default action or a
    // sanitizer's handler.
    sigfillset(&other_signals);
    for(int s : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP, SIGABRT, SIGSYS})
        sigdelset(&other_signals, s);
    for(int i = 0; i < kLineCount; i++)
        sigdelset(&other_signals, SignalOf(i));

    for(int i = 0; i < kLineCount; i++)
    {
        struct sigaction sa = {};
        sa.sa_sigaction     = Dispatch;
        sa.sa_flags         = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        for(int j = 0; j < kLineCount; j++)
            if(LevelMask(i) >> j & 1)
                sigaddset(&sa.sa_mask, SignalOf(j));
        if(sigaction(SignalOf(i), &sa, &old_actions[i]) != 0)
            Fail("can't install the interrupt signal handlers");
    }
    handlers_installed = true;
}

void* FirmwareThread(void* arg)
{
    Boot& boot = *static_cast<Boot*>(arg);
    fw_self.store(pthread_self());
    // Timer expiries are wanted to the nanosecond, not batched to the default 50 us slack.
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);

    for(int i = 0; i < kLineCount; i++)
    {
        sigevent sev            = {};
        sev.sigev_notify        = SIGEV_THREAD_ID;
        sev.sigev_signo         = SignalOf(i);
        sev._sigev_un._tid      = gettid();
        if(timer_create(CLOCK_MONOTONIC, &sev, &timers[i]) != 0)
            Fail("can't create the interrupt timers");
        timer_created[i] = true;
    }
    threaded.store(true);

    // Carry over anything raised, scheduled or started before the thread existed.
    const uint64_t now = NowNs();
    for(int i = 0; i < kLineCount; i++)
    {
        LineState& l = lines[i];
        for(int n = l.pending.exchange(0); n > 0; n--)
            Raise(Line(i));
        const uint64_t due = l.due_ns.exchange(0);
        const uint64_t period = l.period_ns.load();
        if(period != 0)
        {
            const uint64_t next = l.next_ns.load();
            ArmTimer(i, next > now ? next - now : 1, period);
        }
        else if(due != 0)
            ArmTimer(i, due > now ? due - now : 1, 0);
    }

    // Park jumps back here with every signal blocked.
    if(_setjmp(park_jmp) == 0)
    {
        running.store(true);
        boot.ready.store(true);
        ApplyMask();
        exit_code.store(boot.firmware_main());

        // On the chip, returning from main lands in an endless loop in the startup code, and no
        // interrupt runs any more.
        exited.store(true);
        for(int i = 0; i < kLineCount; i++)
            ArmTimer(i, 0, 0);
        sigset_t all;
        sigfillset(&all);
        pthread_sigmask(SIG_SETMASK, &all, nullptr);
    }

    // Halted, or main returned: the thread ends, and Halt or Shutdown joins it.
    running.store(false);
    thread_ended.store(true);
    return nullptr;
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
    if(!threaded.load())
    {
        lines[int(line)].pending.fetch_add(1);
        return;
    }
    sigval value = {};
    if(thread_ended.load() || pthread_sigqueue(fw_thread, SignalOf(int(line)), value) != 0)
        lines[int(line)].dropped.fetch_add(1);
}

void Schedule(Line line, uint64_t delay_ns)
{
    // At least 1 ns so that a zero delay still reads as "scheduled".
    if(threaded.load())
        ArmTimer(int(line), delay_ns ? delay_ns : 1, 0);
    else
        lines[int(line)].due_ns.store(NowNs() + (delay_ns ? delay_ns : 1));
}

void SetPeriodic(Line line, uint64_t period_ns)
{
    LineState& l = lines[int(line)];
    l.next_ns.store(NowNs() + period_ns);
    l.period_ns.store(period_ns);
    if(threaded.load() && !halt_requested.load())
        ArmTimer(int(line), period_ns, period_ns);
}

bool InInterrupt()
{
    return depth.load() > 0;
}

void DisableIrq()
{
    if(OnFirmwareThread())
        pthread_sigmask(SIG_BLOCK, &irq_signals, nullptr);
    primask.store(1);
}

void EnableIrq()
{
    primask.store(0);
    ApplyMask();
}

uint32_t GetPrimask()
{
    return primask.load();
}

void SetPrimask(uint32_t value)
{
    if(value & 1)
        DisableIrq();
    else
        EnableIrq();
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
    ApplyMask();
}

void SetAllMasked(bool masked)
{
    all_masked.store(masked);
    ApplyMask();
}

uint64_t NowNs()
{
    if(manual_clock.load())
        return manual_now.load();
    CheckHalt();
    const uint64_t epoch = Epoch(); // first, so the first call reads 0 rather than wrapping
    return MonotonicNs() - epoch;
}

size_t ServiceInterrupts()
{
    if(threaded.load())
        Fail("ServiceInterrupts is for single-threaded use, before the firmware starts");
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

namespace
{
// Sleeps until the clock reads t_ns. Signals end the sleep early; their handlers have run by
// then, and the loop goes back to sleep. Long delays wake every 10 ms to notice a halt request.
void SleepThreaded(uint64_t t_ns)
{
    for(uint64_t now = NowNs(); now < t_ns; now = NowNs())
    {
        const uint64_t wake  = t_ns - now > 10000000u ? now + 10000000u : t_ns;
        const timespec until = ToTimespec(Epoch() + wake);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr);
    }
}
} // namespace

void SleepUntil(uint64_t t_ns)
{
    // On the firmware thread, higher-priority interrupts still preempt a delay inside a handler,
    // as on the chip. Another thread (a test waiting on the firmware) just sleeps.
    if(threaded.load())
    {
        CheckHalt();
        SleepThreaded(t_ns);
        return;
    }

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
        SleepThreaded(t_ns);
        return;
    }

    if(!manual_clock.load())
    {
        for(uint64_t now = NowNs(); now < t_ns; now = NowNs())
        {
            ServiceInterrupts();
            const timespec ts = ToTimespec(t_ns - now);
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
    if(started.load())
        Fail("the manual clock is only for single-threaded use, before the firmware starts");
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

void StartFirmware(int (*firmware_main)())
{
    if(started.exchange(true))
        Fail("the firmware can only be started once; a firmware library can be power-cycled");
    if(manual_clock.load())
        Fail("turn the manual clock off before starting the firmware");
    if(InInterrupt())
        Fail("can't start the firmware from an interrupt handler");

    board::MapSdram();
    InstallHandlers();

    // The new thread inherits this mask: everything blocked until it has set itself up.
    sigset_t all, saved;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &saved);

    static Boot    boot;
    boot.firmware_main = firmware_main;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 << 20);
    const int err = pthread_create(&fw_thread, &attr, FirmwareThread, &boot);
    pthread_attr_destroy(&attr);
    pthread_sigmask(SIG_SETMASK, &saved, nullptr);
    if(err != 0)
        Fail("can't create the firmware thread");

    while(!boot.ready.load())
    {
        const timespec ts{0, 100000};
        nanosleep(&ts, nullptr);
    }
}

namespace
{
// Joins the firmware thread once it has stopped running; it returns right after.
void JoinFirmwareThread()
{
    if(started.load() && !running.load() && !joined.exchange(true))
        pthread_join(fw_thread, nullptr);
}
} // namespace

bool FirmwareRunning()
{
    return running.load();
}

bool FirmwareStarted()
{
    return started.load();
}

bool HaltFirmware(uint32_t timeout_ms)
{
    if(!threaded.load())
        return true;
    halt_requested.store(true);
    for(int i = 0; i < kLineCount; i++)
        ArmTimer(i, 0, 0);
    const uint64_t deadline = MonotonicNs() + uint64_t(timeout_ms) * 1000000u;
    while(running.load())
    {
        if(MonotonicNs() >= deadline)
            return false;
        const timespec ts{0, 200000};
        nanosleep(&ts, nullptr);
    }
    JoinFirmwareThread();
    return true;
}

void EnterStop()
{
    sleeps.fetch_add(1);
    if(!OnFirmwareThread())
        return;
    SetAllMasked(true);
    const uint64_t seq = wake_seq.load();
    sleeping.store(true);
    while(wake_seq.load() == seq)
    {
        // Nothing runs during STOP, so it is a safe place to halt even inside a handler.
        if(halt_requested.load())
            Park();
        const timespec ts{0, 1000000};
        nanosleep(&ts, nullptr);
    }
    sleeping.store(false);
    SetAllMasked(false);
}

void Wake()
{
    wake_seq.fetch_add(1);
}

bool Shutdown()
{
    if(running.load())
        return false;
    for(int i = 0; i < kLineCount; i++)
        if(timer_created[i])
        {
            timer_delete(timers[i]);
            timer_created[i] = false;
        }
    // The thread can't be in firmware code any more: it returned after parking or after main.
    JoinFirmwareThread();
    if(handlers_installed)
    {
        for(int i = 0; i < kLineCount; i++)
            sigaction(SignalOf(i), &old_actions[i], nullptr);
        handlers_installed = false;
    }
    return true;
}

FirmwareState GetFirmwareState()
{
    return {started.load(),
            running.load(),
            sleeping.load(),
            exited.load(),
            exit_code.load(),
            sleeps.load()};
}

LineStats GetLineStats(Line line)
{
    const LineState& l = lines[int(line)];
    return {l.count.load(), l.total_ns.load(), l.max_ns.load(), l.dropped.load()};
}

} // namespace daisycola::mcu
