#include "board/board.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "daisycola/host.h"
#include "mcu/vmcu.h"

namespace daisycola
{
namespace
{
constexpr int kPorts      = 11; // PORTA..PORTK
constexpr int kPinsPerPort = 16;
constexpr int kMaxChains   = 8;
constexpr int kMaxEncoders = 16;

struct Encoder;
struct Chain;

struct PinSlot
{
    std::atomic<uint8_t>  mode{uint8_t(board::PinMode::kInput)};
    std::atomic<uint8_t>  pull{uint8_t(board::PinPull::kNone)};
    std::atomic<int8_t>   external{-1}; // -1 = not driven by the host
    std::atomic<uint8_t>  output{0};
    std::atomic<uint32_t> changes{0};

    // Wiring, set up before the firmware runs.
    std::atomic<Chain*>   clk_of{nullptr};
    std::atomic<Chain*>   latch_of{nullptr};
    std::atomic<Chain*>   data_of{nullptr};
    std::atomic<Encoder*> encoder{nullptr};
    std::atomic<bool>     encoder_is_b{false};
};

PinSlot pins[kPorts][kPinsPerPort];

PinSlot* Slot(daisy::Pin pin)
{
    if(!pin.IsValid() || int(pin.port) >= kPorts)
        return nullptr;
    return &pins[pin.port][pin.pin];
}

void Fail(const char* message)
{
    std::fprintf(stderr, "daisycola: %s\n", message);
    std::abort();
}

// ---- Encoders -----------------------------------------------------------------------------------
//
// The line levels are a pure function of time and one atomic word the host writes: the phase was
// p0 at t0 and moves one Gray-code step per dwell towards the target. Reads are lock-free and can
// happen from interrupts.

struct Encoder
{
    Line                  a, b;
    uint32_t              dwell_us;
    std::atomic<uint64_t> schedule{0}; // t0_us:40 | p0:12 | target:12

    static constexpr uint64_t kTimeMask = (1ull << 40) - 1;

    static uint64_t Pack(uint64_t t0, int p0, int target)
    {
        return (t0 & kTimeMask) << 24 | (uint64_t(p0) & 0xfff) << 12 | (uint64_t(target) & 0xfff);
    }
    static int SignExtend12(uint64_t v) { return int(int16_t(uint16_t(v << 4)) >> 4); }

    int Phase(uint64_t now_us, int* target_out = nullptr) const
    {
        const uint64_t s      = schedule.load(std::memory_order_acquire);
        const uint64_t t0     = s >> 24;
        const int      p0     = SignExtend12(s >> 12);
        const int      target = SignExtend12(s);
        if(target_out)
            *target_out = target;
        const uint64_t steps = ((now_us - t0) & kTimeMask) / dwell_us;
        const int      dist  = target - p0;
        if(uint64_t(dist < 0 ? -dist : dist) <= steps)
            return target;
        return dist > 0 ? p0 + int(steps) : p0 - int(steps);
    }

    // Phase 0..3 = (A,B) (1,1), (1,0), (0,0), (0,1): B falls first going up.
    bool Level(bool line_b, uint64_t now_us) const
    {
        const int p = Phase(now_us) & 3;
        return line_b ? (p == 0 || p == 3) : (p == 0 || p == 1);
    }

    void Queue(int detents, uint64_t now_us)
    {
        uint64_t s = schedule.load();
        for(;;)
        {
            int       target = 0;
            const int now_p  = Phase(now_us, &target);
            // Keep the 12-bit counters small: the levels only depend on the phase modulo 4.
            const int shift = now_p / 4 * 4;
            const uint64_t next
                = Pack(now_us, now_p - shift, target - shift + 4 * detents);
            if(schedule.compare_exchange_weak(s, next))
                return;
        }
    }
};

Encoder           encoders[kMaxEncoders];
std::atomic<int>  encoder_count{0};

uint64_t NowUs()
{
    return mcu::NowNs() / 1000;
}

// ---- CD4021 chains ------------------------------------------------------------------------------

struct Chain
{
    daisy::Pin            clk, latch, data;
    int                   chips;
    std::atomic<uint64_t> inputs{~0ull};

    // Shift-register state. Only the MCU thread touches it.
    uint64_t reg       = 0;
    bool     clk_level = false;
    bool     latch_hi  = false;

    // Encoder lines wired to inputs of this chain.
    struct Overlay
    {
        int      bit;
        Encoder* encoder;
        bool     is_b;
    };
    Overlay          overlays[2 * kMaxEncoders];
    std::atomic<int> overlay_count{0};

    uint64_t Mask() const { return chips == 8 ? ~0ull : (1ull << (8 * chips)) - 1; }

    uint64_t ParallelInputs() const
    {
        uint64_t  v = inputs.load(std::memory_order_acquire);
        const int n = overlay_count.load(std::memory_order_acquire);
        if(n > 0)
        {
            const uint64_t now = NowUs();
            for(int i = 0; i < n; i++)
            {
                const Overlay& o = overlays[i];
                if(o.encoder->Level(o.is_b, now))
                    v |= 1ull << o.bit;
                else
                    v &= ~(1ull << o.bit);
            }
        }
        return v & Mask();
    }

    // P/S high loads the parallel inputs (asynchronously, for as long as it stays high).
    void OnLatch(bool level)
    {
        latch_hi = level;
        if(level)
            reg = ParallelInputs();
    }

    // A rising clock edge with P/S low shifts every stage one place towards the data pin. The
    // serial input of the first chip is tied low.
    void OnClk(bool level)
    {
        if(level && !clk_level && !latch_hi)
            reg = (reg << 1) & Mask();
        clk_level = level;
    }

    bool Output()
    {
        if(latch_hi)
            reg = ParallelInputs();
        return reg >> (8 * chips - 1) & 1;
    }
};

Chain            chains[kMaxChains];
std::atomic<int> chain_count{0};

Chain& GetChain(int id)
{
    if(id < 0 || id >= chain_count.load())
        Fail("no such CD4021 chain");
    return chains[id];
}

void CheckWiring()
{
    if(mcu::FirmwareRunning())
        Fail("wire chips and encoders before the firmware starts");
}

} // namespace

// ---- Firmware side ------------------------------------------------------------------------------

void board::ConfigurePin(daisy::Pin pin, PinMode mode, PinPull pull)
{
    PinSlot* s = Slot(pin);
    if(!s)
        return;
    s->pull.store(uint8_t(pull));
    s->mode.store(uint8_t(mode));
}

bool board::ReadPin(daisy::Pin pin)
{
    PinSlot* s = Slot(pin);
    if(!s)
        return false;
    if(s->mode.load() == uint8_t(PinMode::kOutput))
        return s->output.load();
    if(Encoder* e = s->encoder.load())
        return e->Level(s->encoder_is_b.load(), NowUs());
    if(Chain* c = s->data_of.load())
        return c->Output();
    const int8_t ext = s->external.load();
    if(ext >= 0)
        return ext;
    return s->pull.load() == uint8_t(PinPull::kUp);
}

void board::WritePin(daisy::Pin pin, bool level)
{
    PinSlot* s = Slot(pin);
    if(!s)
        return;
    if(s->output.exchange(level) != level)
        s->changes.fetch_add(1);
    if(Chain* c = s->latch_of.load())
        c->OnLatch(level);
    if(Chain* c = s->clk_of.load())
        c->OnClk(level);
}

// ---- Host side ----------------------------------------------------------------------------------

void SetPin(daisy::Pin pin, bool level)
{
    if(PinSlot* s = Slot(pin))
        s->external.store(level ? 1 : 0);
}

void ReleasePin(daisy::Pin pin)
{
    if(PinSlot* s = Slot(pin))
        s->external.store(-1);
}

bool GetPin(daisy::Pin pin)
{
    return board::ReadPin(pin);
}

uint32_t GetPinChanges(daisy::Pin pin)
{
    PinSlot* s = Slot(pin);
    return s ? s->changes.load() : 0;
}

int AttachSr4021(daisy::Pin clk, daisy::Pin latch, daisy::Pin data, int chips)
{
    CheckWiring();
    if(chips < 1 || chips > 8)
        Fail("a CD4021 chain has 1 to 8 chips");
    PinSlot *sc = Slot(clk), *sl = Slot(latch), *sd = Slot(data);
    if(!sc || !sl || !sd)
        Fail("CD4021 chain on an invalid pin");
    const int id = chain_count.load();
    if(id >= kMaxChains)
        Fail("too many CD4021 chains");
    Chain& c = chains[id];
    c.clk    = clk;
    c.latch  = latch;
    c.data   = data;
    c.chips  = chips;
    // ShiftRegister4021 can run parallel chains off one clock and latch. CHOMPI doesn't, so
    // daisycola supports one chain per clock and latch pin for now.
    if(sc->clk_of.load() || sl->latch_of.load())
        Fail("only one CD4021 chain per clock and latch pin is supported");
    chain_count.store(id + 1);
    sc->clk_of.store(&c);
    sl->latch_of.store(&c);
    sd->data_of.store(&c);
    return id;
}

void SetSrInputs(int chain, uint64_t levels)
{
    GetChain(chain).inputs.store(levels, std::memory_order_release);
}

void SetSrInput(int chain, int bit, bool level)
{
    Chain& c = GetChain(chain);
    if(level)
        c.inputs.fetch_or(1ull << bit);
    else
        c.inputs.fetch_and(~(1ull << bit));
}

uint64_t GetSrInputs(int chain)
{
    return GetChain(chain).inputs.load();
}

namespace
{
void WireLine(const Line& line, Encoder* e, bool is_b)
{
    if(line.chain < 0)
    {
        PinSlot* s = Slot(line.pin);
        if(!s)
            Fail("encoder on an invalid pin");
        s->encoder_is_b.store(is_b);
        s->encoder.store(e);
        return;
    }
    Chain&    c = GetChain(line.chain);
    const int n = c.overlay_count.load();
    if(line.bit < 0 || line.bit >= 8 * c.chips)
        Fail("encoder on a missing CD4021 input");
    c.overlays[n] = {line.bit, e, is_b};
    c.overlay_count.store(n + 1, std::memory_order_release);
}

bool SameLine(const Line& x, const Line& y)
{
    return x.chain == y.chain && (x.chain < 0 ? x.pin == y.pin : x.bit == y.bit);
}

Encoder& GetEncoder(int id)
{
    if(id < 0 || id >= encoder_count.load())
        Fail("no such encoder");
    return encoders[id];
}
} // namespace

int AttachEncoder(Line a, Line b, uint32_t dwell_us)
{
    CheckWiring();
    const int id = encoder_count.load();
    if(id >= kMaxEncoders)
        Fail("too many encoders");
    if(dwell_us == 0)
        Fail("encoder dwell must be positive");
    Encoder& e = encoders[id];
    e.a        = a;
    e.b        = b;
    e.dwell_us = dwell_us;
    e.schedule.store(Encoder::Pack(NowUs(), 0, 0));
    WireLine(a, &e, false);
    WireLine(b, &e, true);
    encoder_count.store(id + 1);
    return id;
}

void QueueDetents(int encoder, int detents)
{
    GetEncoder(encoder).Queue(detents, NowUs());
}

void QueueDetents(Line a, Line b, int detents)
{
    for(int i = 0; i < encoder_count.load(); i++)
        if(SameLine(encoders[i].a, a) && SameLine(encoders[i].b, b))
            return QueueDetents(i, detents);
    QueueDetents(AttachEncoder(a, b), detents);
}

int PendingDetents(int encoder)
{
    Encoder&  e      = GetEncoder(encoder);
    int       target = 0;
    const int phase  = e.Phase(NowUs(), &target);
    const int left   = target - phase;
    return left >= 0 ? (left + 3) / 4 : -((-left + 3) / 4);
}

} // namespace daisycola
