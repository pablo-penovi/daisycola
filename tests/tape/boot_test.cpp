// Phase 4: TAPE, unchanged, boots headless on the firmware thread from its factory SD card.
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <thread>

#include "daisy_seed.h"
#include "daisycola/host.h"
#include "../unit/test_util.h"

using namespace daisy;

int tape_main();

// TAPE's own boot flags (chompi_main.cpp). The test only reads them.
extern bool booting;
extern bool rainbow_done;

#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))
// The firmware writes these plain bools and the test polls them: a race only as far as the test
// goes, so tell ThreadSanitizer.
extern "C" void
AnnotateBenignRaceSized(const char* file, int line, const volatile void* mem, long size, const char* desc);
#define BENIGN_RACE(var) AnnotateBenignRaceSized(__FILE__, __LINE__, &(var), sizeof(var), #var)
#else
#define BENIGN_RACE(var)
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
// The MP2722 battery charger at I2C 0x3F: on USB power, battery fine. TAPE reads six status
// registers from 0x11; VIN_GD (0x12 bit 6) keeps it out of its low-battery lockout.
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

bool Booted()
{
    return !__atomic_load_n(&booting, __ATOMIC_RELAXED)
           && __atomic_load_n(&rainbow_done, __ATOMIC_RELAXED);
}
} // namespace

TEST(TapeBoot, BootsFromTheFactoryCardAndRunsItsInterrupts)
{
    BENIGN_RACE(booting);
    BENIGN_RACE(rainbow_done);
    namespace fs = std::filesystem;

    // A copy of the factory card, missing a _double file. TAPE makes it again at boot.
    TempDir        dir;
    const fs::path card = dir.path() / "card";
    fs::copy(DAISYCOLA_TAPE_CARD_DIR, card);
    const uintmax_t double_size = fs::file_size(card / "cubbi_b2_double.wav");
    fs::remove(card / "cubbi_b2_double.wav");
    daisycola::SdInsert(card);

    Mp2722 charger;
    daisycola::AttachI2CDevice(0, 0x3f, &charger);
    daisycola::SetPin(seed::D31, true); // MP2722 interrupt line, active low: idle
    daisycola::AttachSr4021(seed::D8, seed::D7, seed::D9, 5);   // keys: all inputs high = released
    daisycola::AttachSr4021(seed::D22, seed::D23, seed::D19, 1); // encoders 1-4

    const auto t0 = std::chrono::steady_clock::now();
    daisycola::Start(tape_main);
    while(!Booted())
    {
        ASSERT_TRUE(daisycola::GetBoardState().running);
        ASSERT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(60)) << "boot timed out";
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const daisycola::AudioFormat format = daisycola::GetAudioFormat();
    EXPECT_TRUE(format.started);
    EXPECT_EQ(format.sample_rate, 48000.f);
    EXPECT_EQ(format.block_size, 24u);
    EXPECT_EQ(format.channels, 4u);
    EXPECT_GT(daisycola::GetAudioStats().blocks, 1000u);
    EXPECT_GT(daisycola::GetIrqStats(daisycola::Irq::kTim4).count, 1000u) << "SD callback";
    EXPECT_GT(daisycola::GetIrqStats(daisycola::Irq::kI2c).count, 0u) << "battery reads";
    EXPECT_FALSE(daisycola::GetBoardState().sleeping);

    // The LEDs: 25 key LEDs (GRB) on TIM3 channel 2, 10 PTH LEDs (RGB) on TIM5 channel 4.
    static daisycola::DmaFrame frame;
    daisycola::Rgb             leds[32];
    ASSERT_TRUE(daisycola::GetDmaFrame(3, 2, frame));
    EXPECT_EQ(daisycola::Ws2812Decode(frame, daisycola::ColorOrder::kGrb, leds, 32), 25u);
    ASSERT_TRUE(daisycola::GetDmaFrame(5, 4, frame));
    EXPECT_EQ(daisycola::Ws2812Decode(frame, daisycola::ColorOrder::kRgb, leds, 32), 10u);

    ASSERT_TRUE(daisycola::Halt(3000));
    ASSERT_TRUE(fs::exists(card / "cubbi_b2_double.wav")) << "TAPE made the missing _double file";
    EXPECT_EQ(fs::file_size(card / "cubbi_b2_double.wav"), double_size);
}
