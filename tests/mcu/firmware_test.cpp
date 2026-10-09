// Phase 4: the firmware thread. Each test runs a small libDaisy program as firmware, with real
// interrupts, and watches it from the test's thread.
//
// A process can only run firmware once, so each test needs its own process. ctest runs them that
// way; running the binary directly runs the first test and skips the rest.
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

#include "daisy_seed.h"
#include "daisycola/host.h"
#include "../unit/test_util.h"

using namespace daisy;
using daisycola::MidiPort;

#if defined(__SANITIZE_THREAD__)
#define UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UNDER_TSAN 1
#endif
#endif

namespace
{
class Firmware : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if(daisycola::GetBoardState().started)
            GTEST_SKIP() << "firmware already ran in this process; run each test on its own";
    }
    void TearDown() override
    {
        if(daisycola::GetBoardState().running)
            EXPECT_TRUE(daisycola::Halt());
    }
};

template <typename Pred>
bool WaitFor(Pred pred, int timeout_ms = 3000)
{
    const auto deadline
        = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while(!pred())
    {
        if(std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

void Idle()
{
    for(;;)
        System::Delay(1);
}

// A register-file I2C device, enough to answer a read.
class FakeChip : public daisycola::I2CDevice
{
  public:
    bool Write(const uint8_t*, size_t) override { return true; }
    bool Read(uint8_t* data, size_t size) override
    {
        std::memset(data, 0x5a, size);
        return true;
    }
};

DaisySeed hw;

I2CHandle MakeI2c()
{
    I2CHandle::Config cfg;
    cfg.mode           = I2CHandle::Config::Mode::I2C_MASTER;
    cfg.periph         = I2CHandle::Config::Peripheral::I2C_1;
    cfg.speed          = I2CHandle::Config::Speed::I2C_100KHZ;
    cfg.pin_config.scl = seed::D11;
    cfg.pin_config.sda = seed::D12;
    I2CHandle i2c;
    i2c.Init(cfg);
    return i2c;
}

// TIM5 is 32-bit and counts at 200 MHz without boost.
void StartTim5(TimerHandle& tim, uint32_t period_us, TimerHandle::PeriodElapsedCallback cb)
{
    TimerHandle::Config cfg;
    cfg.periph     = TimerHandle::Config::Peripheral::TIM_5;
    cfg.dir        = TimerHandle::Config::CounterDir::UP;
    cfg.period     = period_us * 200 - 1;
    cfg.enable_irq = true;
    tim.Init(cfg);
    tim.SetCallback(cb, nullptr);
    tim.Start();
}
} // namespace

// ---- Interrupt order ----------------------------------------------------------------------------

namespace order
{
char              seen[16];
std::atomic<int>  n_seen{0};
std::atomic<bool> done{false};

void Record(char c)
{
    for(int i = 0; i < n_seen.load(); i++)
        if(seen[i] == c)
            return;
    seen[n_seen.load()] = c;
    n_seen.fetch_add(1);
}

void Audio(AudioHandle::InputBuffer, AudioHandle::OutputBuffer, size_t)
{
    Record('A');
}
void I2cDone(void*, I2CHandle::Result)
{
    Record('I');
}
void UartRx(uint8_t*, size_t, void*, UartHandler::Result)
{
    Record('U');
}
void LedDmaDone(void*)
{
    Record('D');
}
void UsbRx(uint8_t*, size_t, void*)
{
    Record('B');
}
void Tim(void*)
{
    Record('T');
}

int Main()
{
    hw.Init();
    I2CHandle i2c = MakeI2c();

    UartHandler         uart;
    UartHandler::Config uart_cfg;
    uart_cfg.periph = UartHandler::Config::Peripheral::USART_1;
    uart_cfg.mode   = UartHandler::Config::Mode::TX_RX;
    uart.Init(uart_cfg);
    static uint8_t uart_buf[16];
    uart.DmaListenStart(uart_buf, sizeof uart_buf, UartRx, nullptr);

    MidiUsbTransport         usb;
    MidiUsbTransport::Config usb_cfg;
    usb.Init(usb_cfg);
    usb.StartRx(UsbRx, nullptr);

    TimerHandle         tim3;
    TimerHandle::Config tim3_cfg;
    tim3_cfg.periph = TimerHandle::Config::Peripheral::TIM_3;
    tim3_cfg.period = 249;
    tim3.Init(tim3_cfg);
    TimChannel         led;
    TimChannel::Config led_cfg;
    led_cfg.tim = &tim3;
    led_cfg.pin = seed::D2;
    led.Init(led_cfg);

    // Make every source pending with interrupts off, then let them all in at once.
    __disable_irq();
    hw.StartAudio(Audio);
    TimerHandle tim5;
    StartTim5(tim5, 10, Tim);
    static uint8_t rx[1];
    i2c.ReceiveDma(0x3f, rx, 1, I2cDone, nullptr);
    static uint32_t duty[4] = {20, 10, 20, 10};
    led.StartDma(duty, 4, LedDmaDone, nullptr);
    const uint8_t clock = 0xf8;
    daisycola::WriteMidiIn(MidiPort::kUart, &clock, 1);
    daisycola::WriteMidiIn(MidiPort::kUsb, &clock, 1);
    System::DelayUs(2000);
    __enable_irq();

    System::Delay(2);
    done.store(true);
    Idle();
    return 0;
}
} // namespace order

TEST_F(Firmware, PendingInterruptsRunInPriorityOrder)
{
    FakeChip chip;
    daisycola::AttachI2CDevice(0, 0x3f, &chip);
    daisycola::SetUsbConnected(true);
    daisycola::Start(order::Main);
    ASSERT_TRUE(WaitFor([] { return order::done.load(); }));
    // Audio, I2C, UART, LED DMA, USB (all priority 0, by stream order), then the timer.
    EXPECT_EQ(std::string(order::seen, order::n_seen.load()), "AIUDBT");
}

// ---- Preemption ---------------------------------------------------------------------------------

namespace preempt
{
std::atomic<bool> in_tim{false}, in_audio{false};
std::atomic<int>  tim_runs{0}, audio_runs{0}, uart_runs{0};
std::atomic<int>  audio_inside_tim{0}, tim_inside_audio{0}, uart_inside_audio{0};
std::atomic<bool> audio_busy_done{false};

void Tim(void*)
{
    if(in_audio.load())
        tim_inside_audio.fetch_add(1);
    if(tim_runs.fetch_add(1) < 3)
    {
        in_tim.store(true);
        System::DelayUs(3000);
        in_tim.store(false);
    }
}

void Audio(AudioHandle::InputBuffer, AudioHandle::OutputBuffer, size_t)
{
    if(in_tim.load())
        audio_inside_tim.fetch_add(1);
    if(audio_runs.fetch_add(1) == 50)
    {
        // Masking and unmasking inside a handler returns to the handler's level: the timer and
        // the UART still have to wait.
        {
            ScopedIrqBlocker block;
        }
        __disable_irq();
        __enable_irq();
        in_audio.store(true);
        System::DelayUs(3000);
        in_audio.store(false);
        audio_busy_done.store(true);
    }
}

void UartRx(uint8_t*, size_t, void*, UartHandler::Result)
{
    if(in_audio.load())
        uart_inside_audio.fetch_add(1);
    uart_runs.fetch_add(1);
}

int Main()
{
    hw.Init();
    UartHandler         uart;
    UartHandler::Config uart_cfg;
    uart_cfg.periph = UartHandler::Config::Peripheral::USART_1;
    uart.Init(uart_cfg);
    static uint8_t buf[16];
    uart.DmaListenStart(buf, sizeof buf, UartRx, nullptr);
    hw.StartAudio(Audio);
    TimerHandle tim5;
    StartTim5(tim5, 1000, Tim);
    Idle();
    return 0;
}
} // namespace preempt

TEST_F(Firmware, OnlyHigherPriorityInterruptsPreempt)
{
    using namespace preempt;
    daisycola::Start(Main);
    ASSERT_TRUE(WaitFor([] { return in_audio.load() || audio_busy_done.load(); }));
    const uint8_t clock = 0xf8;
    daisycola::WriteMidiIn(MidiPort::kUart, &clock, 1);
    ASSERT_TRUE(WaitFor([] { return audio_busy_done.load() && uart_runs.load() > 0; }));
    ASSERT_TRUE(WaitFor([] { return tim_runs.load() > 10; }));

#ifndef UNDER_TSAN
    // ThreadSanitizer defers signals and runs the handlers with every signal blocked, so
    // interrupts never nest under it.
    EXPECT_GT(audio_inside_tim.load(), 0) << "audio (priority 0) preempts the timer (15)";
#endif
    EXPECT_EQ(tim_inside_audio.load(), 0) << "the timer never preempts audio";
    EXPECT_EQ(uart_inside_audio.load(), 0) << "equal priorities don't preempt";
    const auto audio = daisycola::GetIrqStats(daisycola::Irq::kAudio);
    EXPECT_GE(audio.max_ns, 3000000u);
}

// ---- Main loop, critical sections and NVIC masks ------------------------------------------------

namespace critical
{
std::atomic<bool> in_critical{false};
std::atomic<int>  violations{0}, audio_runs{0}, tim_runs{0};
std::atomic<int>  handlers_during_main{0};
std::atomic<bool> errno_kept{false}, done{false};
std::atomic<int>  audio_masked_delta{-1}, tim_masked_delta{-1}, audio_after_delta{-1};

void Check()
{
    if(in_critical.load())
        violations.fetch_add(1);
    errno = EIO; // the interrupted code must not see this
}

void Audio(AudioHandle::InputBuffer, AudioHandle::OutputBuffer, size_t)
{
    Check();
    audio_runs.fetch_add(1);
}

void Tim(void*)
{
    Check();
    tim_runs.fetch_add(1);
}

int Main()
{
    hw.Init();
    hw.StartAudio(Audio);
    TimerHandle tim5;
    StartTim5(tim5, 100, Tim);

    errno = 0;
    for(int i = 0; i < 100; i++)
    {
        {
            ScopedIrqBlocker block;
            in_critical.store(true);
            System::DelayUs(300);
            in_critical.store(false);
        }
        const int before = audio_runs.load() + tim_runs.load();
        System::DelayUs(300);
        handlers_during_main.fetch_add(audio_runs.load() + tim_runs.load() - before);
    }
    errno_kept.store(errno == 0);

    // Disabling the audio DMA interrupt in the NVIC holds audio back but not the timer; enabling
    // it again runs the pending interrupt.
    HAL_NVIC_DisableIRQ(DMA1_Stream0_IRQn);
    System::Delay(1);
    const int a0 = audio_runs.load(), t0 = tim_runs.load();
    System::Delay(20);
    audio_masked_delta.store(audio_runs.load() - a0);
    tim_masked_delta.store(tim_runs.load() - t0);
    const int a1 = audio_runs.load();
    HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);
    System::Delay(2);
    audio_after_delta.store(audio_runs.load() - a1);

    done.store(true);
    Idle();
    return 0;
}
} // namespace critical

TEST_F(Firmware, CriticalSectionsAndNvicMasksHoldInterruptsBack)
{
    using namespace critical;
    daisycola::Start(Main);
    ASSERT_TRUE(WaitFor([] { return done.load(); }, 10000));
    EXPECT_EQ(violations.load(), 0) << "no handler ran inside ScopedIrqBlocker";
    EXPECT_GT(handlers_during_main.load(), 100) << "handlers interrupt the main loop's delays";
    EXPECT_TRUE(errno_kept.load()) << "handlers don't clobber the main loop's errno";
    EXPECT_EQ(audio_masked_delta.load(), 0);
    EXPECT_GT(tim_masked_delta.load(), 100);
    EXPECT_GT(audio_after_delta.load(), 0);
}

// ---- Audio --------------------------------------------------------------------------------------

namespace audio
{
void Levels(AudioHandle::InputBuffer, AudioHandle::OutputBuffer out, size_t size)
{
    for(size_t c = 0; c < 4; c++)
        for(size_t i = 0; i < size; i++)
            out[c][i] = 0.25f * float(c + 1);
}

// TAPE's audio setup: a second codec on SAI2, 24-frame blocks.
void InitTapeAudio()
{
    hw.Init(true);
    SaiHandle::Config cfg;
    cfg.periph    = SaiHandle::Config::Peripheral::SAI_2;
    cfg.sr        = SaiHandle::Config::SampleRate::SAI_48KHZ;
    cfg.bit_depth = SaiHandle::Config::BitDepth::SAI_24BIT;
    static SaiHandle sai2;
    sai2.Init(cfg);
    AudioHandle::Config audio_cfg;
    audio_cfg.blocksize  = 24;
    audio_cfg.samplerate = SaiHandle::Config::SampleRate::SAI_48KHZ;
    audio_cfg.postgain   = 1.f;
    hw.audio_handle.Init(audio_cfg, hw.AudioSaiHandle(), sai2);
}

int LevelsMain()
{
    InitTapeAudio();
    hw.StartAudio(Levels);
    Idle();
    return 0;
}

void PassThrough(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    for(size_t c = 0; c < 2; c++)
        for(size_t i = 0; i < size; i++)
            out[c][i] = in[c][i];
}

int PassThroughMain()
{
    hw.Init();
    hw.StartAudio(PassThrough);
    Idle();
    return 0;
}
} // namespace audio

TEST_F(Firmware, InternalAudioClockRunsAtTheSampleRate)
{
    daisycola::Start(audio::LevelsMain);
    ASSERT_TRUE(WaitFor([] { return daisycola::GetAudioFormat().started; }));
    const daisycola::AudioFormat format = daisycola::GetAudioFormat();
    EXPECT_EQ(format.sample_rate, 48000.f);
    EXPECT_EQ(format.block_size, 24u);
    EXPECT_EQ(format.channels, 4u);

    std::vector<float> ch[4];
    for(auto& c : ch)
        c.resize(4096);
    float* out[4] = {ch[0].data(), ch[1].data(), ch[2].data(), ch[3].data()};

    daisycola::ReadAudio(out, 4096); // drop what's there
    const auto t0     = std::chrono::steady_clock::now();
    size_t     frames = 0;
    while(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        frames += daisycola::ReadAudio(out, 4096);
    }
    const double seconds
        = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_NEAR(frames / seconds, 48000.0, 48000.0 * 0.05);

    EXPECT_EQ(ch[0][0], 0.25f);
    EXPECT_EQ(ch[2][0], 0.75f);
    EXPECT_EQ(ch[3][0], s242f(f2s24(0.999985f))) << "libDaisy clips output just below 1.0";
    EXPECT_EQ(daisycola::GetAudioStats().overruns, 0u);
}

TEST_F(Firmware, HostAudioClockPassesBlocksThroughWithTwoBlocksOfLatency)
{
    daisycola::SetAudioClock(daisycola::AudioClock::kHost);
    daisycola::Start(audio::PassThroughMain);
    ASSERT_TRUE(WaitFor([] { return daisycola::GetAudioFormat().started; }));
    const size_t block = daisycola::GetAudioFormat().block_size;
    ASSERT_EQ(block, 48u);

    // Buffers of 100 frames: not a multiple of the block size. The ramp is exact in 24 bits.
    constexpr size_t kFrames = 100, kCalls = 50;
    std::vector<float> input(kFrames * kCalls), output(kFrames * kCalls);
    for(size_t n = 0; n < input.size(); n++)
        input[n] = float(n) / 65536.f;
    for(size_t k = 0; k < kCalls; k++)
    {
        const float* in[4]  = {&input[k * kFrames], &input[k * kFrames], nullptr, nullptr};
        float*       out[4] = {&output[k * kFrames], nullptr, nullptr, nullptr};
        ASSERT_TRUE(daisycola::ProcessAudio(in, out, kFrames)) << "call " << k;
    }
    for(size_t n = 0; n < output.size(); n++)
        ASSERT_EQ(output[n], n < 2 * block ? 0.f : input[n - 2 * block]) << "frame " << n;
    EXPECT_EQ(daisycola::GetAudioStats().underruns, 0u);

    // Input is clipped to the codec's range.
    float        loud[kFrames], out0[kFrames];
    const float* in[4]  = {loud, nullptr, nullptr, nullptr};
    float*       out[4] = {out0, nullptr, nullptr, nullptr};
    for(float& s : loud)
        s = 1.5f;
    daisycola::ProcessAudio(in, out, kFrames);
    daisycola::ProcessAudio(in, out, kFrames);
    EXPECT_EQ(out0[kFrames - 1], s242f(f2s24(0.999985f)));
}

// ---- MIDI ---------------------------------------------------------------------------------------

namespace midi
{
std::atomic<int> usb_send_failures{0};

int Main()
{
    hw.Init();
    MidiUartHandler         uart;
    MidiUartHandler::Config uart_cfg;
    uart.Init(uart_cfg);
    uart.StartReceive();

    MidiUsbHandler         usb;
    MidiUsbHandler::Config usb_cfg;
    usb_cfg.transport_config.periph = MidiUsbTransport::Config::EXTERNAL;
    usb.Init(usb_cfg);
    usb.Listen();

    // Echo: a note on from either port comes back a semitone up on both; a CC from USB comes
    // back on the UART as CC 1.
    for(;;)
    {
        uart.Listen();
        while(uart.HasEvents() || usb.HasEvents())
        {
            const bool      from_usb = !uart.HasEvents();
            const MidiEvent e        = from_usb ? usb.PopEvent() : uart.PopEvent();
            if(e.type == NoteOn)
            {
                uart.SendNoteOn(e.channel, e.data[0] + 1, e.data[1]);
                if(!usb.SendNoteOn(e.channel, e.data[0] + 1, e.data[1]))
                    usb_send_failures.fetch_add(1);
            }
            else if(e.type == ControlChange && from_usb)
                uart.SendCC(e.channel, 1, e.data[1]);
        }
        System::DelayUs(100);
    }
    return 0;
}

std::vector<uint8_t> Read(MidiPort port, size_t n)
{
    std::vector<uint8_t> got;
    WaitFor([&] {
        uint8_t      b[64];
        const size_t k = daisycola::ReadMidiOut(port, b, sizeof b);
        got.insert(got.end(), b, b + k);
        return got.size() >= n;
    });
    return got;
}
} // namespace midi

TEST_F(Firmware, MidiGoesThroughTheFirmwaresOwnParserAndBackOut)
{
    using V = std::vector<uint8_t>;
    daisycola::Start(midi::Main);

    const uint8_t note[] = {0x90, 60, 100};
    ASSERT_EQ(daisycola::WriteMidiIn(MidiPort::kUart, note, 3), 3u);
    EXPECT_EQ(midi::Read(MidiPort::kUart, 3), (V{0x90, 61, 100}));
    ASSERT_TRUE(WaitFor([] { return midi::usb_send_failures.load() == 1; }))
        << "USB sends fail while unplugged";
    EXPECT_EQ(daisycola::WriteMidiIn(MidiPort::kUsb, note, 3), 0u) << "and USB input is lost";

    daisycola::SetUsbConnected(true);
    const uint8_t note2[] = {0x91, 62, 1};
    daisycola::WriteMidiIn(MidiPort::kUart, note2, 3);
    EXPECT_EQ(midi::Read(MidiPort::kUart, 3), (V{0x91, 63, 1}));
    EXPECT_EQ(midi::Read(MidiPort::kUsb, 3), (V{0x91, 63, 1}));

    const uint8_t cc[] = {0xb2, 7, 99};
    daisycola::WriteMidiIn(MidiPort::kUsb, cc, 3);
    EXPECT_EQ(midi::Read(MidiPort::kUart, 3), (V{0xb2, 1, 99}));
    EXPECT_EQ(midi::usb_send_failures.load(), 1);
}

// ---- STOP mode, halting and returning from main -------------------------------------------------

namespace power
{
std::atomic<int> stage{0};
std::atomic<int> audio_runs{0};

void Audio(AudioHandle::InputBuffer, AudioHandle::OutputBuffer, size_t)
{
    audio_runs.fetch_add(1);
}

int StopMain()
{
    hw.Init();
    hw.StartAudio(Audio);
    System::Delay(5);
    stage.store(1);
    HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);
    stage.store(2);
    Idle();
    return 0;
}

int ReturnMain()
{
    System::Delay(1);
    return 7;
}
} // namespace power

TEST_F(Firmware, StopModeMasksEverythingUntilWoken)
{
    using namespace power;
    daisycola::Start(StopMain);
    ASSERT_TRUE(WaitFor([] { return daisycola::GetBoardState().sleeping; }));
    const int before = audio_runs.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(audio_runs.load(), before) << "no interrupts while asleep";
    EXPECT_EQ(stage.load(), 1);

    daisycola::Wake();
    ASSERT_TRUE(WaitFor([] { return stage.load() == 2; }));
    EXPECT_FALSE(daisycola::GetBoardState().sleeping);
    EXPECT_EQ(daisycola::GetBoardState().sleeps, 1u);
    EXPECT_TRUE(WaitFor([&] { return audio_runs.load() > before + 10; }));
}

TEST_F(Firmware, HaltParksTheFirmwareAndFreesTheSdCard)
{
    daisycola::Start(power::StopMain);
    ASSERT_TRUE(WaitFor([] { return power::stage.load() == 1; }));
    daisycola::Wake();
    ASSERT_TRUE(WaitFor([] { return power::stage.load() == 2; }));

    ASSERT_TRUE(daisycola::Halt());
    const daisycola::BoardState state = daisycola::GetBoardState();
    EXPECT_TRUE(state.started);
    EXPECT_FALSE(state.running);
    EXPECT_FALSE(state.exited);
    const uint64_t blocks = daisycola::GetAudioStats().blocks;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_EQ(daisycola::GetAudioStats().blocks, blocks) << "timers stop";

    TempDir tmp;
    daisycola::SdCreateImage(tmp / "card.img", 64 << 20);
}

TEST_F(Firmware, ReturningFromMainStopsTheFirmware)
{
    daisycola::Start(power::ReturnMain);
    ASSERT_TRUE(WaitFor([] { return daisycola::GetBoardState().exited; }));
    EXPECT_EQ(daisycola::GetBoardState().exit_code, 7);
    EXPECT_TRUE(WaitFor([] { return !daisycola::GetBoardState().running; }));
}
