// Phase 3: decoding WS2812 PWM duty buffers.
#include <gtest/gtest.h>
#include <vector>

#include "daisycola/host.h"

using daisycola::ColorOrder;
using daisycola::Rgb;

namespace
{
// TAPE's LED timing: prescaler 8 (PSC 7), ARR 35 at 240 MHz = 1.2 us bits; 1 = 20, 0 = 10 ticks.
constexpr uint32_t kPsc = 7, kArr = 35, kOne = 20, kZero = 10;

void PushByte(std::vector<uint32_t>& buf, uint8_t v)
{
    for(int i = 7; i >= 0; i--)
        buf.push_back(v >> i & 1 ? kOne : kZero);
}

void PushOff(std::vector<uint32_t>& buf, int leds)
{
    buf.insert(buf.end(), size_t(24 * leds), 0u);
}
} // namespace

TEST(Ws2812, DecodesGrbBitsMsbFirst)
{
    std::vector<uint32_t> buf;
    PushByte(buf, 0x12); // G
    PushByte(buf, 0xA5); // R
    PushByte(buf, 0x0F); // B
    PushByte(buf, 0xFF);
    PushByte(buf, 0x00);
    PushByte(buf, 0x80);
    Rgb leds[4];
    ASSERT_EQ(daisycola::Ws2812Decode(buf.data(), buf.size(), kArr, kPsc, ColorOrder::kGrb, leds, 4),
              2u);
    EXPECT_EQ(leds[0].r, 0xA5);
    EXPECT_EQ(leds[0].g, 0x12);
    EXPECT_EQ(leds[0].b, 0x0F);
    EXPECT_EQ(leds[1].r, 0x00);
    EXPECT_EQ(leds[1].g, 0xFF);
    EXPECT_EQ(leds[1].b, 0x80);

    ASSERT_EQ(daisycola::Ws2812Decode(buf.data(), buf.size(), kArr, kPsc, ColorOrder::kRgb, leds, 4),
              2u);
    EXPECT_EQ(leds[0].r, 0x12);
    EXPECT_EQ(leds[0].g, 0xA5);
}

// A long low stretch latches the chain; LEDs after it start again from the first. Short gaps
// and the trailing reset don't.
TEST(Ws2812, LowLineLongerThanResetLatches)
{
    std::vector<uint32_t> buf;
    PushOff(buf, 6); // 173 us low: reset
    PushByte(buf, 1);
    PushByte(buf, 2);
    PushByte(buf, 3);
    PushOff(buf, 6);
    Rgb leds[2];
    EXPECT_EQ(daisycola::Ws2812Decode(buf.data(), buf.size(), kArr, kPsc, ColorOrder::kGrb, leds, 2),
              1u);
    EXPECT_EQ(leds[0].g, 1);

    // 6 bits low = 7.2 us: no reset, so the zero-duty words are simply skipped.
    std::vector<uint32_t> gap;
    PushByte(gap, 0xAA);
    gap.insert(gap.end(), 6, 0u);
    PushByte(gap, 0xBB);
    PushByte(gap, 0xCC);
    EXPECT_EQ(daisycola::Ws2812Decode(gap.data(), gap.size(), kArr, kPsc, ColorOrder::kGrb, leds, 2),
              1u);
    EXPECT_EQ(leds[0].g, 0xAA);
    EXPECT_EQ(leds[0].r, 0xBB);
}
