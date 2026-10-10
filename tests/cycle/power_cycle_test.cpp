// Power cycles: firmware built as a library, loaded, run, unloaded and loaded again in one process
// through daisycola_host.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <unistd.h>

#include "daisy_seed.h"
#include "daisycola/host.h"

using namespace daisycola;

#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
// The SDRAM at 0xC0000000 is in ASan's shadow gap; see src/dev/sdram.cpp.
extern "C" const char* __asan_default_options()
{
    return "protect_shadow_gap=0";
}
#endif

namespace
{
constexpr int kLines = 9; // daisycola's interrupt lines, SIGRTMIN + 2 onwards

template <typename Pred>
bool WaitFor(Pred pred, int timeout_ms = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while(!pred())
    {
        if(std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Where the counter firmware reports its counts. A host object: it outlives every cycle.
class Reports : public I2CDevice
{
  public:
    std::atomic<int> runs{0}, constructions{0}, statics{0}, writes{0};

    bool Write(const uint8_t* data, size_t size) override
    {
        if(size == 3)
        {
            runs.store(data[0]);
            constructions.store(data[1]);
            statics.store(data[2]);
            writes.fetch_add(1);
        }
        return true;
    }
    bool Read(uint8_t*, size_t) override { return false; }
};

void StartCounter(Reports& reports)
{
    reports.writes.store(0);
    AttachI2CDevice(0, 0x42, &reports);
    SetAudioClock(AudioClock::kHost);
    Start();
}

bool Loaded(const char* path)
{
    void* handle = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    if(handle)
        dlclose(handle);
    return handle != nullptr;
}

std::string Status(const char* key)
{
    std::ifstream in("/proc/self/status");
    for(std::string line; std::getline(in, line);)
        if(line.rfind(key, 0) == 0)
            return line;
    return "";
}

// Under newer AddressSanitizer versions 0xC0000000 is ASan's own memory, which daisycola uses
// in place rather than mapping it (src/dev/sdram.cpp).
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
constexpr bool kCheckSdram = false;
#else
constexpr bool kCheckSdram = true;
#endif

bool SdramMapped()
{
    std::ifstream in("/proc/self/maps");
    for(std::string line; std::getline(in, line);)
        if(line.rfind("c0000000-", 0) == 0)
            return true;
    return false;
}

// POSIX timers of this process, or -1 if the kernel doesn't list them.
int PosixTimers()
{
    std::ifstream in("/proc/self/timers");
    if(!in)
        return -1;
    int n = 0;
    for(std::string line; std::getline(in, line);)
        n += line.rfind("ID:", 0) == 0;
    return n;
}

size_t ResidentBytes()
{
    std::ifstream in("/proc/self/statm");
    size_t        size = 0, resident = 0;
    in >> size >> resident;
    return resident * size_t(sysconf(_SC_PAGESIZE));
}

// Runs a host-clocked audio period. True if it carried the firmware's 0.25.
bool HearsFirmware()
{
    static float l[64], r[64];
    float*       out[kMaxAudioChannels] = {l, r, nullptr, nullptr};
    ProcessAudio(nullptr, out, 64);
    return l[63] > 0.2f && r[63] > 0.2f;
}
} // namespace

TEST(PowerCycle, StartsTheFirmwareFromScratchEveryTime)
{
    Reports reports;
    LoadFirmware(COUNTER_FW);
    for(int cycle = 0; cycle < 5; cycle++)
    {
        StartCounter(reports);
        ASSERT_TRUE(WaitFor([&] { return reports.writes.load() > 0; })) << "cycle " << cycle;
        EXPECT_EQ(reports.runs.load(), 1) << "a global, cycle " << cycle;
        EXPECT_EQ(reports.constructions.load(), 1) << "a constructor, cycle " << cycle;
        EXPECT_EQ(reports.statics.load(), 1) << "a static local, cycle " << cycle;
        EXPECT_TRUE(GetBoardState().running);
        ASSERT_NO_THROW(PowerCycle());
        EXPECT_FALSE(GetBoardState().started) << "a bare board after the cycle";
    }
    UnloadFirmware();
    EXPECT_FALSE(Loaded(COUNTER_FW));
}

TEST(PowerCycle, LeavesNothingBehind)
{
    struct sigaction before[kLines];
    for(int i = 0; i < kLines; i++)
        sigaction(SIGRTMIN + 2 + i, nullptr, &before[i]);
    // ThreadSanitizer starts a thread of its own with the first one created: count from there.
    std::thread([] {}).join();
    const std::string threads = Status("Threads:");
    const int         timers  = PosixTimers();

    Reports reports;
    LoadFirmware(COUNTER_FW);
    StartCounter(reports);
    ASSERT_TRUE(WaitFor([&] { return reports.writes.load() > 0; }));
    if(kCheckSdram)
        EXPECT_TRUE(SdramMapped());
    EXPECT_NE(Status("Threads:"), threads) << "the firmware thread runs";
    UnloadFirmware();

    EXPECT_FALSE(Loaded(COUNTER_FW)) << "dlopen(RTLD_NOLOAD) finds nothing";
    if(kCheckSdram)
        EXPECT_FALSE(SdramMapped());
    EXPECT_EQ(Status("Threads:"), threads) << "the firmware thread was joined";
    if(timers >= 0)
        EXPECT_EQ(PosixTimers(), timers);
    for(int i = 0; i < kLines; i++)
    {
        struct sigaction now;
        sigaction(SIGRTMIN + 2 + i, nullptr, &now);
        EXPECT_EQ(now.sa_handler, before[i].sa_handler) << "signal handler " << i;
    }
}

TEST(PowerCycle, MemoryDoesNotGrow)
{
    Reports reports;
    LoadFirmware(COUNTER_FW);
    size_t settled = 0;
    for(int cycle = 0; cycle < 12; cycle++)
    {
        StartCounter(reports);
        ASSERT_TRUE(WaitFor([&] { return reports.writes.load() > 0; }));
        PowerCycle();
        if(cycle == 2)
            settled = ResidentBytes();
    }
    UnloadFirmware();
    EXPECT_LT(ResidentBytes(), settled + (4u << 20)) << "under 4 MB over nine cycles";
}

TEST(PowerCycle, AudioIsSilentWhileUnloaded)
{
    float  l[64], r[64];
    float* out[kMaxAudioChannels] = {l, r, nullptr, nullptr};
    std::fill(l, l + 64, 1.f);
    EXPECT_FALSE(ProcessAudio(nullptr, out, 64)) << "no library";
    EXPECT_EQ(l[0], 0.f);
    EXPECT_EQ(l[63], 0.f);

    Reports reports;
    LoadFirmware(COUNTER_FW);
    StartCounter(reports);
    ASSERT_TRUE(WaitFor(HearsFirmware));

    // The host's audio thread keeps going through power cycles: silence, then the firmware again.
    // It runs at the sample rate, as a real one does: 64 frames every 1.33 ms.
    std::atomic<bool> stop{false};
    std::atomic<int>  periods{0}, other{0};
    std::thread       audio([&] {
        float      al[64], ar[64];
        float*     aout[kMaxAudioChannels] = {al, ar, nullptr, nullptr};
        auto       next                    = std::chrono::steady_clock::now();
        const auto period                  = std::chrono::microseconds(1333);
        while(!stop.load())
        {
            next += period;
            std::this_thread::sleep_until(next);
            ProcessAudio(nullptr, aout, 64);
            for(float v : al)
                if(v != 0.f && (v < 0.2f || v > 0.3f))
                    other.fetch_add(1);
            periods.fetch_add(1);
        }
    });
    bool kept_going = true;
    for(int cycle = 0; cycle < 3 && kept_going; cycle++)
    {
        PowerCycle();
        StartCounter(reports);
        const int now = periods.load();
        kept_going    = WaitFor([&] { return periods.load() > now + 10; });
    }
    stop.store(true);
    audio.join();
    EXPECT_TRUE(kept_going) << "the audio thread got stuck";
    EXPECT_EQ(other.load(), 0) << "only silence or the firmware's output";

    UnloadFirmware();
    std::fill(l, l + 64, 1.f);
    EXPECT_FALSE(ProcessAudio(nullptr, out, 64));
    EXPECT_EQ(l[63], 0.f);
}

TEST(PowerCycle, CallsWithoutALibraryDoNothing)
{
    const BoardState state = GetBoardState();
    EXPECT_FALSE(state.started);
    EXPECT_FALSE(state.running);
    const uint8_t note[] = {0x90, 60, 100};
    EXPECT_EQ(WriteMidiIn(MidiPort::kUart, note, 3), 0u);
    EXPECT_FALSE(GetAudioFormat().started);
    EXPECT_TRUE(Halt());
    SetPin(daisy::seed::D1, true);
    EXPECT_FALSE(GetPin(daisy::seed::D1));
    EXPECT_DEATH(Start(), "needs a firmware library");
    EXPECT_DEATH(SdInsert("/tmp"), "needs a firmware library");
}

TEST(PowerCycle, ReportsLoadErrors)
{
    EXPECT_THROW(PowerCycle(), FirmwareError) << "nothing loaded";
    EXPECT_THROW(LoadFirmware("/nonexistent/libnothing.so"), FirmwareError);
    LoadFirmware(COUNTER_FW);
    EXPECT_THROW(LoadFirmware(COUNTER_FW), FirmwareError) << "already loaded";
    UnloadFirmware();
}

TEST(PowerCycle, KeepsAFirmwareThatWontHaltRunning)
{
    LoadFirmware(SPIN_FW);
    Start();
    try
    {
        PowerCycle(100);
        FAIL() << "the firmware never halts";
    }
    catch(const FirmwareError& e)
    {
        EXPECT_NE(std::string(e.what()).find("didn't halt"), std::string::npos) << e.what();
    }
    EXPECT_TRUE(Loaded(SPIN_FW));
    EXPECT_TRUE(GetBoardState().running) << "still loaded and running";
}
