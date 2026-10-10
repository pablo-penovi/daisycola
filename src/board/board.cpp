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
// The lines step through Gray-code phases towards a target the host sets. A phase only steps once
// the firmware has read it in two different milliseconds and it has been held for the dwell, so
// reads that come in bursts (audio blocks run back to back on the host's audio thread) see every
// phase. The whole state is one atomic word: reads are lock-free and can happen from interrupts.

struct Encoder
{
    EncoderLine           a, b;
    uint32_t              dwell_us;
    std::atomic<uint64_t> state{0}; // since_us:24 | read_ms:14 | reads:2 | phase:12 | target:12

    // Reads of a phase, in different milliseconds, before it can step. CHOMPI's encoder code
    // samples its lines at most once a millisecond; its shift-register decoder needs a phase in two
    // samples in a row.
    static constexpr int      kMinReads  = 2;
    static constexpr uint64_t kSinceMask = (1ull << 24) - 1;
    static constexpr uint64_t kMsMask    = (1ull << 14) - 1;

    struct State
    {
        uint64_t since_us; // when the phase started, low 24 bits
        uint64_t read_ms;  // the millisecond of the last read, low 14 bits
        int      reads;    // reads of the phase in different milliseconds, up to 3
        int      phase;
        int      target;
    };

    static int SignExtend12(uint64_t v) { return int(int16_t(uint16_t(v << 4)) >> 4); }

    static uint64_t Pack(const State& st)
    {
        return (st.since_us & kSinceMask) << 40 | (st.read_ms & kMsMask) << 26 | uint64_t(st.reads) << 24
               | (uint64_t(st.phase) & 0xfff) << 12 | (uint64_t(st.target) & 0xfff);
    }

    static State Unpack(uint64_t s)
    {
        return {s >> 40, s >> 26 & kMsMask, int(s >> 24 & 3), SignExtend12(s >> 12), SignExtend12(s)};
    }

    void Reset(uint64_t now_us) { state.store(Pack({now_us, now_us / 1000, 3, 0, 0})); }

    // The firmware reads the lines: counts the read, steps if the phase is due, and returns the
    // phase the lines show.
    int Read(uint64_t now_us)
    {
        uint64_t s = state.load(std::memory_order_acquire);
        for(;;)
        {
            State          st = Unpack(s);
            const uint64_t ms = now_us / 1000 & kMsMask;
            if(ms == st.read_ms)
                return st.phase; // counted already; steps only happen on a new millisecond
            st.read_ms = ms;
            if(st.phase != st.target && st.reads >= kMinReads
               && ((now_us - st.since_us) & kSinceMask) >= dwell_us)
            {
                st.phase += st.target > st.phase ? 1 : -1;
                st.since_us = now_us;
                st.reads    = 1; // this read sees the new phase
            }
            else if(st.reads < 3)
                st.reads++;
            if(state.compare_exchange_weak(s, Pack(st), std::memory_order_acq_rel))
                return st.phase;
        }
    }

    // Phase 0..3 = (A,B) (1,1), (1,0), (0,0), (0,1): B falls first going up.
    bool Level(bool line_b, uint64_t now_us)
    {
        const int p = Read(now_us) & 3;
        return line_b ? (p == 0 || p == 3) : (p == 0 || p == 1);
    }

    void Queue(int detents)
    {
        uint64_t s = state.load();
        for(;;)
        {
            State st = Unpack(s);
            // Keep the 12-bit counters small: the levels only depend on the phase modulo 4.
            const int shift = st.phase / 4 * 4;
            st.phase -= shift;
            st.target += 4 * detents - shift;
            if(state.compare_exchange_weak(s, Pack(st)))
                return;
        }
    }

    // Phases left to go.
    int Left() const
    {
        const State st = Unpack(state.load(std::memory_order_acquire));
        return st.target - st.phase;
    }
};

Encoder           encoders[kMaxEncoders];
std::atomic<int>  encoder_count{0};

uint64_t NowUs()
{
    return mcu::NowNs() / 1000;
}

// ---- CD4021 chains ------------------------------------------------------------------------------

// One input of a chain with `hold_reads`: the level the firmware sees and the changes queued
// behind it. The next change shows once the firmware has read the level in `hold_reads` different
// milliseconds. Levels alternate, so a count of changes is enough. One atomic word, as for
// encoders: the host queues and the firmware reads with compare-and-swap.
struct HeldInput
{
    std::atomic<uint64_t> state{0}; // level:1 | reads:5 | read_ms:14 | pending:16

    static constexpr uint64_t kMsMask     = (1ull << 14) - 1;
    static constexpr int      kMaxReads   = 31;
    static constexpr int      kMaxPending = 0xffff;

    struct State
    {
        bool     level;
        int      reads;   // reads of the level in different milliseconds, up to hold_reads
        uint64_t read_ms; // the millisecond of the last read, low 14 bits
        int      pending; // changes queued behind the level
    };

    static uint64_t Pack(const State& st)
    {
        return uint64_t(st.level) << 35 | uint64_t(st.reads) << 30 | (st.read_ms & kMsMask) << 16
               | uint64_t(st.pending);
    }

    static State Unpack(uint64_t s)
    {
        return {bool(s >> 35 & 1), int(s >> 30 & 31), s >> 16 & kMsMask, int(s & 0xffff)};
    }

    // A level the firmware hasn't read yet counts as read enough: it has been there all along.
    void Reset(bool level, int hold_reads, uint64_t now_ms)
    {
        state.store(Pack({level, hold_reads, (now_ms - 1) & kMsMask, 0}));
    }

    void Queue()
    {
        uint64_t s = state.load();
        for(;;)
        {
            State st = Unpack(s);
            // Dropping a press and its release keeps the levels in step.
            st.pending = st.pending < kMaxPending ? st.pending + 1 : st.pending - 1;
            if(state.compare_exchange_weak(s, Pack(st)))
                return;
        }
    }

    // The firmware reads the input: counts the read, moves on to the next level if it's due, and
    // returns the level the input shows.
    bool Read(int hold_reads, uint64_t now_ms)
    {
        uint64_t s = state.load(std::memory_order_acquire);
        for(;;)
        {
            State          st = Unpack(s);
            const uint64_t ms = now_ms & kMsMask;
            if(ms == st.read_ms)
                return st.level; // counted already
            st.read_ms = ms;
            if(st.pending > 0 && st.reads >= hold_reads)
            {
                st.level = !st.level;
                st.pending--;
                st.reads = 1; // this read sees the new level
            }
            else if(st.reads < hold_reads)
                st.reads++;
            if(state.compare_exchange_weak(s, Pack(st), std::memory_order_acq_rel))
                return st.level;
        }
    }

    int Pending() const { return Unpack(state.load(std::memory_order_acquire)).pending; }
};

struct Chain
{
    daisy::Pin            clk, latch, data;
    int                   chips;
    int                   hold_reads = 0;
    std::atomic<uint64_t> inputs{~0ull};
    HeldInput             held[64];

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

    int Bits() const { return 8 * chips; }

    uint64_t ParallelInputs()
    {
        uint64_t v = 0;
        if(hold_reads > 0)
        {
            const uint64_t now_ms = NowUs() / 1000;
            for(int i = 0; i < Bits(); i++)
                v |= uint64_t(held[i].Read(hold_reads, now_ms)) << i;
        }
        else
            v = inputs.load(std::memory_order_acquire);
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

    // The host changed these inputs: queue a change on each, if they're held.
    void Changed(uint64_t bits)
    {
        if(hold_reads == 0)
            return;
        bits &= Mask();
        for(int i = 0; i < Bits(); i++)
            if(bits >> i & 1)
                held[i].Queue();
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

int AttachSr4021(daisy::Pin clk, daisy::Pin latch, daisy::Pin data, int chips, int hold_reads)
{
    CheckWiring();
    if(chips < 1 || chips > 8)
        Fail("a CD4021 chain has 1 to 8 chips");
    if(hold_reads < 0 || hold_reads > HeldInput::kMaxReads)
        Fail("a CD4021 chain holds its inputs for 0 to 31 reads");
    PinSlot *sc = Slot(clk), *sl = Slot(latch), *sd = Slot(data);
    if(!sc || !sl || !sd)
        Fail("CD4021 chain on an invalid pin");
    const int id = chain_count.load();
    if(id >= kMaxChains)
        Fail("too many CD4021 chains");
    Chain& c = chains[id];
    c.clk        = clk;
    c.latch      = latch;
    c.data       = data;
    c.chips      = chips;
    c.hold_reads = hold_reads;
    for(HeldInput& h : c.held)
        h.Reset(true, hold_reads, NowUs() / 1000);
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
    Chain& c = GetChain(chain);
    c.Changed(c.inputs.exchange(levels, std::memory_order_acq_rel) ^ levels);
}

void SetSrInput(int chain, int bit, bool level)
{
    Chain&         c    = GetChain(chain);
    const uint64_t mask = 1ull << bit;
    const uint64_t old  = level ? c.inputs.fetch_or(mask) : c.inputs.fetch_and(~mask);
    c.Changed((old ^ (level ? mask : 0)) & mask);
}

uint64_t GetSrInputs(int chain)
{
    return GetChain(chain).inputs.load();
}

int PendingSrChanges(int chain)
{
    Chain& c     = GetChain(chain);
    int    total = 0;
    if(c.hold_reads > 0)
        for(int i = 0; i < c.Bits(); i++)
            total += c.held[i].Pending();
    return total;
}

namespace
{
void WireLine(const EncoderLine& line, Encoder* e, bool is_b)
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

bool SameLine(const EncoderLine& x, const EncoderLine& y)
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

int AttachEncoder(EncoderLine a, EncoderLine b, uint32_t dwell_us)
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
    e.Reset(NowUs());
    WireLine(a, &e, false);
    WireLine(b, &e, true);
    encoder_count.store(id + 1);
    return id;
}

void QueueDetents(int encoder, int detents)
{
    GetEncoder(encoder).Queue(detents);
}

void QueueDetents(EncoderLine a, EncoderLine b, int detents)
{
    for(int i = 0; i < encoder_count.load(); i++)
        if(SameLine(encoders[i].a, a) && SameLine(encoders[i].b, b))
            return QueueDetents(i, detents);
    QueueDetents(AttachEncoder(a, b), detents);
}

int PendingDetents(int encoder)
{
    const int left = GetEncoder(encoder).Left();
    return left >= 0 ? (left + 3) / 4 : -((-left + 3) / 4);
}

} // namespace daisycola
