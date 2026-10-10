// TAPE as a firmware library: power-cycled 20 times in one process, booting every time.
#include <chrono>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>
#include <unistd.h>

#include "daisy_seed.h"
#include "daisycola/host.h"
#include "../unit/test_util.h"

using namespace daisy;

#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))
// ThreadSanitizer keeps the shadow memory of each unloaded library resident (ld.so unmaps it
// behind its back), so resident memory only means something without it.
constexpr bool kCheckMemory = false;
#else
constexpr bool kCheckMemory = true;
#endif

#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
// TAPE zeroes the SDRAM at 0xC0000000, which is in ASan's shadow gap; see src/dev/sdram.cpp.
extern "C" const char* __asan_default_options()
{
    return "protect_shadow_gap=0";
}
#endif

namespace
{
// The MP2722 battery charger at I2C 0x3F, as in boot_test.cpp. A host object: it's attached again
// after every cycle.
class Mp2722 : public daisycola::I2CDevice
{
  public:
    uint8_t regs[0x20] = {};
    uint8_t ptr        = 0;

    Mp2722() { regs[0x12] = 1 << 6; }

    bool Write(const uint8_t* data, size_t size) override
    {
        ptr = data[0] & 0x1f;
        for(size_t i = 1; i < size; i++)
            regs[(ptr + i - 1) & 0x1f] = data[i];
        return true;
    }
    bool Read(uint8_t* data, size_t size) override
    {
        for(size_t i = 0; i < size; i++)
            data[i] = regs[(ptr + i) & 0x1f];
        return true;
    }
};

// TAPE's boot flags, through tape_probe.cpp. The library is looked up for each poll and let go
// at once: the host keeps no pointer into it across a cycle.
bool Booted()
{
    void* lib = dlopen(TAPE_FW, RTLD_NOW | RTLD_NOLOAD);
    if(!lib)
        return false;
    const auto probe  = reinterpret_cast<int (*)()>(dlsym(lib, "tape_probe_booted"));
    const bool booted = probe && probe();
    dlclose(lib);
    return booted;
}

size_t ResidentBytes()
{
    std::ifstream in("/proc/self/statm");
    size_t        size = 0, resident = 0;
    in >> size >> resident;
    return resident * size_t(sysconf(_SC_PAGESIZE));
}
} // namespace

TEST(TapeCycle, BootsAfterEveryPowerCycle)
{
    namespace fs = std::filesystem;
    TempDir        dir;
    const fs::path card = dir.path() / "card";
    fs::copy(DAISYCOLA_TAPE_CARD_DIR, card);

    Mp2722 charger;
    daisycola::LoadFirmware(TAPE_FW);
    size_t settled = 0;
    for(int cycle = 0; cycle < 20; cycle++)
    {
        daisycola::AttachI2CDevice(0, 0x3f, &charger);
        daisycola::SetPin(seed::D31, true);
        daisycola::AttachSr4021(seed::D8, seed::D7, seed::D9, 5);
        daisycola::AttachSr4021(seed::D22, seed::D23, seed::D19, 1);
        daisycola::SdInsert(card);

        const auto t0 = std::chrono::steady_clock::now();
        daisycola::Start();
        while(!Booted())
        {
            ASSERT_TRUE(daisycola::GetBoardState().running) << "cycle " << cycle;
            ASSERT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(60))
                << "boot timed out, cycle " << cycle;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT_GT(daisycola::GetIrqStats(daisycola::Irq::kTim4).count, 100u) << "cycle " << cycle;
        EXPECT_TRUE(daisycola::GetAudioFormat().started) << "cycle " << cycle;

        ASSERT_NO_THROW(daisycola::PowerCycle(3000)) << "cycle " << cycle;
        EXPECT_FALSE(Booted()) << "TAPE starts over, cycle " << cycle;
        if(cycle == 2)
            settled = ResidentBytes();
    }
    daisycola::UnloadFirmware();
    if(kCheckMemory)
        EXPECT_LT(ResidentBytes(), settled + (16u << 20)) << "resident memory stays put";
}
