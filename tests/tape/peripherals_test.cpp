// Phase 3: daisycola's peripheral models, driven single-threaded by TAPE's own code: the fork's
// ShiftRegister4021, CHOMPI's ChompiEncoder and TAPE's WS2812 LED driver.
#include <gtest/gtest.h>
#include <random>

#include "daisycola/host.h"
#include "encoder.h"
#include "temp_led_stuff.h" // defines TAPE's LED globals: include in this file only

using namespace daisy;
using daisycola::EncoderLine;

namespace
{
class Tape : public ::testing::Test
{
  protected:
    void SetUp() override { daisycola::UseManualClock(true); }
};

// ---- CD4021 -------------------------------------------------------------------------------------

// The fork's per-input debounce (dev/sr_4021.h), restated: it counts up by one per millisecond
// while the input is high, down while low, clamped to 2 * dbc_size; State is "count < dbc_size".
struct ReferenceDebounce
{
    int      dbc_size;
    uint32_t last   = 0;
    bool     updated = false;
    uint16_t count  = 0;
    bool     risen  = false;

    void Update(bool raw, uint32_t now)
    {
        updated = false;
        if(now - last >= 1)
        {
            last    = now;
            updated = true;
            count += raw * 2;
            if(count > 0)
                count -= 1;
            if(count > dbc_size * 2)
                count = dbc_size * 2;
        }
    }
    bool State() const { return count < dbc_size; }
    bool Rising()
    {
        if(!risen && State() && updated)
            return risen = true;
        return false;
    }
    bool Falling()
    {
        if(risen && !State() && updated)
        {
            risen = false;
            return true;
        }
        return false;
    }
};
} // namespace

TEST_F(Tape, Sr4021InputBitsMatchFirmwareIndices)
{
    // TAPE's button chain: five chips on D8 (clock), D7 (latch), D9 (data).
    const int chain = daisycola::AttachSr4021(seed::D8, seed::D7, seed::D9, 5);
    ShiftRegister4021<5, 1>         sr;
    ShiftRegister4021<5, 1>::Config cfg;
    cfg.clk      = seed::D8;
    cfg.latch    = seed::D7;
    cfg.data[0]  = seed::D9;
    cfg.dbc_size = 7;
    sr.Init(cfg);

    for(int bit = 0; bit < 40; bit++)
    {
        daisycola::SetSrInputs(chain, ~(1ull << bit));
        sr.Update();
        for(int i = 0; i < 40; i++)
            ASSERT_EQ(sr.RawState(i), i != bit) << "input " << bit << " read at " << i;
    }
}

TEST_F(Tape, Sr4021DebounceAndEdgesMatchTheForksLogic)
{
    const int chain = daisycola::AttachSr4021(seed::D8, seed::D7, seed::D9, 5);
    ShiftRegister4021<5, 1>         sr;
    ShiftRegister4021<5, 1>::Config cfg;
    cfg.clk      = seed::D8;
    cfg.latch    = seed::D7;
    cfg.data[0]  = seed::D9;
    cfg.dbc_size = 7;
    sr.Init(cfg);

    // Inputs bounce at random; the firmware polls at 2 kHz, as TAPE's audio callback does.
    std::mt19937      rng(1234);
    ReferenceDebounce ref[40];
    for(auto& r : ref)
        r.dbc_size = 7;
    uint64_t levels = ~0ull;
    int      rises = 0, falls = 0;
    for(int step = 0; step < 20000; step++)
    {
        if(rng() % 50 == 0)
            levels ^= 1ull << (rng() % 40);
        daisycola::SetSrInputs(chain, levels);
        sr.Update();
        const uint32_t now = System::GetNow();
        for(int i = 0; i < 40; i++)
        {
            ref[i].Update(levels >> i & 1, now);
            ASSERT_EQ(sr.RawState(i), bool(levels >> i & 1));
            ASSERT_EQ(sr.State(i), ref[i].State()) << "input " << i << " at step " << step;
            const bool rising = sr.RisingEdge(i), falling = sr.FallingEdge(i);
            ASSERT_EQ(rising, ref[i].Rising());
            ASSERT_EQ(falling, ref[i].Falling());
            rises += rising;
            falls += falling;
        }
        daisycola::AdvanceClock(500);
    }
    EXPECT_GT(rises, 50);
    EXPECT_GT(falls, 50);
}

// ---- Encoders -----------------------------------------------------------------------------------

namespace
{
// Polls like TAPE's audio callback (2 kHz) and adds up what ChompiEncoder reports.
template <typename Poll>
int CountIncrements(chompi::ChompiEncoder& enc, Poll poll, int ms)
{
    int total = 0;
    for(int i = 0; i < 2 * ms; i++)
    {
        poll();
        total += enc.Increment();
        daisycola::AdvanceClock(500);
    }
    return total;
}
} // namespace

TEST_F(Tape, EncoderOnPinsCountsQueuedDetents)
{
    // TAPE's encoder 5: A on D0, B on D20, click on D10.
    chompi::ChompiEncoder enc;
    enc.Init(seed::D0, seed::D20, seed::D10);
    const int id = daisycola::AttachEncoder(EncoderLine::OnPin(seed::D0), EncoderLine::OnPin(seed::D20));

    daisycola::QueueDetents(id, 3);
    EXPECT_EQ(daisycola::PendingDetents(id), 3);
    EXPECT_EQ(CountIncrements(enc, [&] { enc.Debounce(); }, 60), 3);
    EXPECT_EQ(daisycola::PendingDetents(id), 0);

    daisycola::QueueDetents(id, -5);
    EXPECT_EQ(CountIncrements(enc, [&] { enc.Debounce(); }, 100), -5);
}

TEST_F(Tape, EncoderOnShiftRegisterCountsQueuedDetents)
{
    // TAPE's encoder 1: A and B on inputs 0 and 1 of the one-chip chain on D22/D23/D19, read with
    // RawState as Hardware::ProcessAllControls does.
    const int chain = daisycola::AttachSr4021(seed::D22, seed::D23, seed::D19, 1);
    ShiftRegister4021<1, 1>         sr;
    ShiftRegister4021<1, 1>::Config cfg;
    cfg.clk      = seed::D22;
    cfg.latch    = seed::D23;
    cfg.data[0]  = seed::D19;
    cfg.dbc_size = 50;
    sr.Init(cfg);
    chompi::ChompiEncoder enc;
    enc.Init(Pin(), Pin(), Pin());

    auto poll = [&] {
        sr.Update();
        enc.Debounce(sr.RawState(0), sr.RawState(1));
    };
    daisycola::QueueDetents(EncoderLine::OnSr(chain, 0), EncoderLine::OnSr(chain, 1), 4);
    EXPECT_EQ(CountIncrements(enc, poll, 80), 4);
    daisycola::QueueDetents(EncoderLine::OnSr(chain, 0), EncoderLine::OnSr(chain, 1), -2);
    daisycola::QueueDetents(EncoderLine::OnSr(chain, 0), EncoderLine::OnSr(chain, 1), -1);
    EXPECT_EQ(CountIncrements(enc, poll, 80), -3);
}

TEST_F(Tape, EncoderCountsReadsThatComeInBursts)
{
    // With the host's audio clock, TAPE's audio callback runs a host period's blocks back to back
    // and then nothing until the next period. Each phase must still reach the decoder.
    const int chain = daisycola::AttachSr4021(seed::D22, seed::D23, seed::D19, 1);
    ShiftRegister4021<1, 1>         sr;
    ShiftRegister4021<1, 1>::Config cfg;
    cfg.clk      = seed::D22;
    cfg.latch    = seed::D23;
    cfg.data[0]  = seed::D19;
    cfg.dbc_size = 50;
    sr.Init(cfg);
    chompi::ChompiEncoder enc;
    enc.Init(Pin(), Pin(), Pin());
    const EncoderLine a = EncoderLine::OnSr(chain, 0), b = EncoderLine::OnSr(chain, 1);

    for(uint32_t period_us : {2667u, 5333u, 21333u})
        for(int detents : {5, -5})
        {
            daisycola::QueueDetents(a, b, detents);
            int total = 0;
            for(int p = 0; p < 100; p++)
            {
                // A period's 1 ms blocks, 20 us apart, then the rest of the period.
                const uint32_t blocks = period_us / 1000;
                for(uint32_t i = 0; i < blocks; i++)
                {
                    sr.Update();
                    enc.Debounce(sr.RawState(0), sr.RawState(1));
                    total += enc.Increment();
                    daisycola::AdvanceClock(20);
                }
                daisycola::AdvanceClock(period_us - 20 * blocks);
            }
            EXPECT_EQ(total, detents) << "period " << period_us << " us";
        }
}

// ---- WS2812 LEDs --------------------------------------------------------------------------------

// TAPE's LedSetup starts the key-LED chain on TIM3 channel 2, and its EndOfLeds callback ping-pongs
// between that chain and the PTH chain on TIM5 channel 4, as on the device.
TEST_F(Tape, TapeLedDriverRoundTrips)
{
    // TAPE boots with seed.Init(true): 480 MHz. LedSetup derives its timer period from that.
    System::Config boost;
    boost.Boost();
    System().Init(boost);
    chompi::LedSetup();
    for(int i = 0; i < 25; i++)
        chompi::SetSmtLed(i, uint8_t(4 * i), uint8_t(200 - 4 * i), 128); // stored as value / 4
    for(int i = 0; i < 10; i++)
        chompi::SetPthLed(i, uint8_t(11 * i), 0, 255); // stored as value / 11
    chompi::fill_led_data();

    daisycola::DmaFrame frame;
    EXPECT_FALSE(daisycola::GetDmaFrame(3, 2, frame)) << "nothing sent before the first transfer ends";

    // SMT: 37 slots * 24 bits * 1.2 us = 1.07 ms; PTH: 22 * 24 * 1.2 us = 0.63 ms. In 10 ms the
    // chains alternate about six times each.
    daisycola::AdvanceClock(10000);

    ASSERT_TRUE(daisycola::GetDmaFrame(3, 2, frame));
    EXPECT_EQ(frame.count, chompi::kOutSmtDataSize);
    EXPECT_EQ(frame.prescaler, 7u);
    EXPECT_EQ(frame.period, 35u);
    EXPECT_GE(frame.sequence, 5u);
    daisycola::Rgb smt[32];
    ASSERT_EQ(daisycola::Ws2812Decode(frame, daisycola::ColorOrder::kGrb, smt, 32), 25u);
    for(int i = 0; i < 25; i++)
    {
        EXPECT_EQ(smt[i].r, i) << i;
        EXPECT_EQ(smt[i].g, (200 - 4 * i) / 4) << i;
        EXPECT_EQ(smt[i].b, 32) << i;
    }

    ASSERT_TRUE(daisycola::GetDmaFrame(5, 4, frame));
    EXPECT_EQ(frame.count, chompi::kOutPthDataSize);
    daisycola::Rgb pth[16];
    ASSERT_EQ(daisycola::Ws2812Decode(frame, daisycola::ColorOrder::kRgb, pth, 16), 10u);
    for(int i = 0; i < 10; i++)
    {
        EXPECT_EQ(pth[i].r, i) << i;
        EXPECT_EQ(pth[i].g, 0) << i;
        EXPECT_EQ(pth[i].b, 23) << i;
    }
}
