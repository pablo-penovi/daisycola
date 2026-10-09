// TimChannel: PWM output with DMA, as CHOMPI uses it to drive WS2812 LED chains.
//
// libDaisy runs every TimChannel DMA transfer on one DMA stream (DMA2 stream 5) with one global
// completion callback. The model does the same: StartDma starts "sending" the duty buffer, and
// after the transfer's real duration (one timer period per word) the DMA interrupt fires. Its
// handler captures the buffer for the host, then calls the firmware's callback.
#include "per/tim_channel.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include "board/board.h"
#include "daisycola/host.h"
#include "mcu/vmcu.h"
#include "per/tim_internal.h"

using namespace daisy;

namespace
{
// A triple buffer per timer channel: the DMA interrupt writes, one host thread at a time reads.
struct FrameSlot
{
    daisycola::DmaFrame frames[3];
    std::atomic<int>    middle{1}; // index | kDirty
    int                 back  = 0; // owned by the writer
    int                 front = 2; // owned by the reader
    uint64_t            sequence = 0;
    std::mutex          reader;

    static constexpr int kDirty = 4;
};

FrameSlot slots[4][4]; // [TIM2..TIM5][channel 1..4]

struct DmaEngine
{
    TimChannel*                            channel = nullptr;
    const uint32_t*                        data    = nullptr;
    size_t                                 size    = 0;
    TimChannel::EndTransmissionFunctionPtr callback = nullptr;
    void*                                  context  = nullptr;
};

DmaEngine engine; // touched only on the MCU thread

void Publish(const TimChannel& ch, const uint32_t* data, size_t size)
{
    const auto&                   cfg  = ch.GetConfig();
    const int                     tim  = int(cfg.tim->GetConfig().periph);
    FrameSlot&                    slot = slots[tim][int(cfg.chn)];
    const daisycola::tim::Registers regs = daisycola::tim::Get(cfg.tim->GetConfig().periph);

    daisycola::DmaFrame& f = slot.frames[slot.back];
    f.timer                = tim + 2;
    f.channel              = int(cfg.chn) + 1;
    f.sequence             = ++slot.sequence;
    f.time_us              = daisycola::mcu::NowNs() / 1000;
    f.prescaler            = regs.psc;
    f.period               = regs.arr;
    f.count                = uint32_t(std::min(size, daisycola::kMaxDmaWords));
    std::memcpy(f.duty, data, f.count * sizeof(uint32_t));

    slot.back = slot.middle.exchange(slot.back | FrameSlot::kDirty) & 3;
}

void TransferComplete(void*)
{
    DmaEngine done = engine;
    engine         = DmaEngine{};
    if(!done.channel)
        return;
    Publish(*done.channel, done.data, done.size);
    if(done.callback)
        done.callback(done.context);
}
} // namespace

void TimChannel::Init(const Config& cfg)
{
    cfg_ = cfg;
    daisycola::board::ConfigurePin(
        cfg_.pin, daisycola::board::PinMode::kOutput, daisycola::board::PinPull::kNone);
    daisycola::mcu::SetHandler(daisycola::mcu::Line::kTimDma, TransferComplete, nullptr);
    daisycola::mcu::SetIrqNumber(daisycola::mcu::Line::kTimDma, DMA2_Stream5_IRQn);
}

// The PWM waveform itself isn't modelled: the host sees the duty buffers of DMA transfers.
void TimChannel::Start() {}

void TimChannel::SetPwm(uint32_t val) {}

void TimChannel::StartDma(void*                      data,
                          size_t                     size,
                          EndTransmissionFunctionPtr callback,
                          void*                      cb_context)
{
    engine.channel  = this;
    engine.data     = static_cast<const uint32_t*>(data);
    engine.size     = size;
    engine.callback = callback;
    engine.context  = cb_context;
    const uint64_t bit_ns
        = daisycola::tim::PeriodNs(daisycola::tim::Get(cfg_.tim->GetConfig().periph));
    daisycola::mcu::Schedule(daisycola::mcu::Line::kTimDma, bit_ns * size);
}

const TimChannel::Config& TimChannel::GetConfig() const
{
    return cfg_;
}

namespace daisycola
{
bool GetDmaFrame(int timer, int channel, DmaFrame& frame)
{
    if(timer < 2 || timer > 5 || channel < 1 || channel > 4)
        return false;
    FrameSlot&                  slot = slots[timer - 2][channel - 1];
    std::lock_guard<std::mutex> lock(slot.reader);
    if(slot.middle.load() & FrameSlot::kDirty)
        slot.front = slot.middle.exchange(slot.front) & 3;
    const DmaFrame& f = slot.frames[slot.front];
    if(f.sequence == 0)
        return false;
    std::memcpy(&frame, &f, offsetof(DmaFrame, duty) + f.count * sizeof(uint32_t));
    return true;
}

size_t Ws2812Decode(const uint32_t* duty,
                    size_t          count,
                    uint32_t        period,
                    uint32_t        prescaler,
                    ColorOrder      order,
                    Rgb*            leds,
                    size_t          max_leds,
                    double          reset_us)
{
    const double bit_us = tim::PeriodNs({prescaler, period}) / 1000.0;
    size_t       n_leds = 0;
    uint32_t     bits = 0, n_bits = 0;
    double       low_us = 0;
    for(size_t i = 0; i < count; i++)
    {
        if(duty[i] == 0)
        {
            low_us += bit_us;
            continue;
        }
        if(low_us >= reset_us) // the chain latched: this bit starts again at the first LED
            n_leds = bits = n_bits = 0;
        low_us = 0;
        bits   = bits << 1 | (2 * uint64_t(duty[i]) > uint64_t(period) + 1 ? 1 : 0);
        if(++n_bits < 24)
            continue;
        if(n_leds < max_leds)
        {
            const uint8_t b0 = bits >> 16, b1 = bits >> 8, b2 = bits;
            leds[n_leds] = order == ColorOrder::kGrb ? Rgb{b1, b0, b2} : Rgb{b0, b1, b2};
        }
        n_leds++;
        bits = n_bits = 0;
    }
    return n_leds;
}

size_t Ws2812Decode(const DmaFrame& frame, ColorOrder order, Rgb* leds, size_t max_leds)
{
    return Ws2812Decode(
        frame.duty, frame.count, frame.period, frame.prescaler, order, leds, max_leds);
}

} // namespace daisycola
