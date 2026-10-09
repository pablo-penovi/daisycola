// AudioHandle on the virtual board.
//
// On the device, the SAI's DMA fills a block of input and empties a block of output, then raises
// the audio interrupt, whose handler converts the codec's integers to floats and calls the
// firmware's callback. Here the audio interrupt takes a block from the host's input ring, does the
// same conversions, calls the callback and puts the block on the output ring. What raises the
// interrupt is either a timer at the block rate (the internal clock) or the host's audio thread
// (ProcessAudio).
#include "hid/audio.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <semaphore.h>

#include "daisycola/host.h"
#include "mcu/vmcu.h"
#include "util/spsc_ring.h"

using namespace daisy;

namespace
{
constexpr size_t kMaxBlock    = 256; // libDaisy's limit: 1024-word DMA buffers, 4 per frame
constexpr size_t kChannels    = daisycola::kMaxAudioChannels;
constexpr size_t kRingFrames  = 8192;
constexpr size_t kChunkFrames = 256;

// Rings hold frames of kChannels interleaved samples, whatever the firmware's channel count.
using Ring = daisycola::SpscRing<float, kRingFrames * kChannels>;
Ring in_ring;  // host -> audio interrupt
Ring out_ring; // audio interrupt -> host

std::atomic<daisycola::AudioClock> clock_source{daisycola::AudioClock::kInternal};
std::atomic<bool>                  started{false};
std::atomic<float>                 sample_rate{0.f};
std::atomic<size_t>                block_size{0};
std::atomic<size_t>                channels{0};

// Host clock: blocks the host has handed over, and blocks the interrupt has processed.
std::atomic<uint64_t> requested{0};
std::atomic<uint64_t> processed{0};
uint64_t              host_frames = 0; // the host audio thread's own count

std::atomic<uint64_t> blocks{0};
std::atomic<uint64_t> underruns{0};
std::atomic<uint64_t> overruns{0};

// Posted once per processed block with the host clock; sem_post is async-signal-safe.
struct BlockSemaphore
{
    sem_t sem;
    BlockSemaphore() { sem_init(&sem, 0, 0); }
} block_done;

void Fail(const char* message)
{
    std::fprintf(stderr, "daisycola: %s\n", message);
    std::abort();
}

// A sample's round trip through the codec's integer format, with libDaisy's own conversions.
// They clip to +-FBIPMAX (0.999985) first. A 24-bit sample travels in the low 24 bits of a 32-bit
// DMA word, and s242f sign-extends from bit 23, so the word is masked as the SAI would.
float Quantise(float x, SaiHandle::Config::BitDepth depth)
{
    switch(depth)
    {
        case SaiHandle::Config::BitDepth::SAI_16BIT: return s162f(f2s16(x));
        case SaiHandle::Config::BitDepth::SAI_24BIT: return s242f(f2s24(x) & 0xffffff);
        case SaiHandle::Config::BitDepth::SAI_32BIT: return s322f(f2s32(x));
    }
    return x;
}
} // namespace

class AudioHandle::Impl
{
  public:
    Config                     config;
    SaiHandle                  sai1, sai2;
    float                      postgain_recip = 1.f;
    float                      output_adjust  = 1.f;
    AudioHandle::AudioCallback callback       = nullptr;

    size_t Channels() const
    {
        return sai1.IsInitialized() && sai2.IsInitialized() ? 4
               : sai1.IsInitialized() || sai2.IsInitialized() ? 2
                                                               : 0;
    }

    void ProcessBlock()
    {
        static float frames[kMaxBlock * kChannels];
        static float fin_buf[kChannels][kMaxBlock];
        static float fout_buf[kChannels][kMaxBlock];

        const size_t                bs    = config.blocksize;
        const size_t                chns  = Channels();
        const SaiHandle::Config::BitDepth depth = sai1.GetConfig().bit_depth;

        const size_t got = in_ring.Read(frames, bs * kChannels) / kChannels;
        if(got < bs)
        {
            if(clock_source.load() == daisycola::AudioClock::kHost)
                underruns.fetch_add(1);
            std::memset(frames + got * kChannels, 0, (bs - got) * kChannels * sizeof(float));
        }

        const float* fin[kChannels];
        float*       fout[kChannels];
        for(size_t c = 0; c < kChannels; c++)
        {
            fin[c]  = fin_buf[c];
            fout[c] = fout_buf[c];
            std::memset(fout_buf[c], 0, bs * sizeof(float));
            for(size_t i = 0; i < bs; i++)
                fin_buf[c][i] = Quantise(frames[i * kChannels + c], depth) * postgain_recip;
        }

        if(callback)
            callback(fin, fout, bs);

        for(size_t i = 0; i < bs; i++)
            for(size_t c = 0; c < kChannels; c++)
                frames[i * kChannels + c]
                    = c < chns ? Quantise(fout_buf[c][i] * output_adjust, depth) : 0.f;
        if(out_ring.Writable() >= bs * kChannels)
            out_ring.Write(frames, bs * kChannels);
        else
            overruns.fetch_add(1);
        blocks.fetch_add(1);
    }

    static void Interrupt(void* context)
    {
        Impl& self = *static_cast<Impl*>(context);
        if(clock_source.load() == daisycola::AudioClock::kInternal)
        {
            self.ProcessBlock();
            return;
        }
        // The host may have handed over several blocks per raise, or raises may have merged.
        while(processed.load() < requested.load())
        {
            self.ProcessBlock();
            processed.fetch_add(1);
            sem_post(&block_done.sem);
        }
    }
};

namespace
{
AudioHandle::Impl audio;
} // namespace

AudioHandle::Result AudioHandle::Init(const Config& config, SaiHandle sai)
{
    pimpl_ = &audio;
    if(config.postgain <= 0.f || !sai.IsInitialized() || config.blocksize > kMaxBlock)
        return Result::ERR;
    audio.config            = config;
    audio.postgain_recip    = 1.f / config.postgain;
    audio.output_adjust     = config.postgain * config.output_compensation;
    audio.sai1              = sai;
    audio.sai2              = SaiHandle();
    audio.config.samplerate = sai.GetConfig().sr;
    return Result::OK;
}

AudioHandle::Result AudioHandle::Init(const Config& config, SaiHandle sai1, SaiHandle sai2)
{
    const Result result = Init(config, sai1);
    audio.sai2          = sai2;
    return result;
}

float AudioHandle::GetSampleRate()
{
    return pimpl_->sai1.GetSampleRate();
}

const AudioHandle::Config& AudioHandle::GetConfig() const
{
    return pimpl_->config;
}

AudioHandle::Result AudioHandle::Start(AudioCallback callback)
{
    Impl& a = *pimpl_;
    if(a.Channels() == 0)
        return Result::ERR;
    // With two SAIs libDaisy runs the callback from SAI2's DMA interrupt, else from SAI1's.
    const bool was_started = started.load();
    a.callback             = callback;
    daisycola::mcu::SetHandler(daisycola::mcu::Line::kAudio, Impl::Interrupt, &a);
    daisycola::mcu::SetIrqNumber(daisycola::mcu::Line::kAudio,
                                 a.sai2.IsInitialized() ? DMA1_Stream3_IRQn : DMA1_Stream0_IRQn);

    const size_t bs = a.config.blocksize;
    sample_rate.store(a.sai1.GetSampleRate());
    block_size.store(bs);
    channels.store(a.Channels());
    if(clock_source.load() == daisycola::AudioClock::kInternal)
    {
        const uint64_t block_ns = uint64_t(bs * 1e9 / a.sai1.GetSampleRate() + 0.5);
        daisycola::mcu::SetPeriodic(daisycola::mcu::Line::kAudio, block_ns);
    }
    else if(!was_started)
    {
        // Two blocks of latency, so that host buffers that aren't a multiple of the block size
        // always find enough output.
        static const float silence[kMaxBlock * kChannels] = {};
        out_ring.Write(silence, bs * kChannels);
        out_ring.Write(silence, bs * kChannels);
    }
    started.store(true);
    return Result::OK;
}

// ---- Host side ----------------------------------------------------------------------------------

namespace daisycola
{
namespace
{
size_t WriteFrames(const float* const* in, size_t frames)
{
    float  chunk[kChunkFrames * kChannels];
    size_t done = 0;
    while(done < frames)
    {
        const size_t n = frames - done < kChunkFrames ? frames - done : kChunkFrames;
        for(size_t i = 0; i < n; i++)
            for(size_t c = 0; c < kChannels; c++)
                chunk[i * kChannels + c] = in && in[c] ? in[c][done + i] : 0.f;
        const size_t wrote = in_ring.Write(chunk, n * kChannels) / kChannels;
        done += wrote;
        if(wrote < n)
            break;
    }
    return done;
}

size_t ReadFrames(float* const* out, size_t frames)
{
    float  chunk[kChunkFrames * kChannels];
    size_t done = 0;
    while(done < frames)
    {
        const size_t want = frames - done < kChunkFrames ? frames - done : kChunkFrames;
        const size_t n    = out_ring.Read(chunk, want * kChannels) / kChannels;
        for(size_t i = 0; i < n; i++)
            for(size_t c = 0; c < kChannels; c++)
                if(out[c])
                    out[c][done + i] = chunk[i * kChannels + c];
        done += n;
        if(n < want)
            break;
    }
    return done;
}

void Silence(float* const* out, size_t from, size_t frames)
{
    for(size_t c = 0; c < kChannels; c++)
        if(out[c])
            std::memset(out[c] + from, 0, (frames - from) * sizeof(float));
}
} // namespace

AudioFormat GetAudioFormat()
{
    return {started.load(), sample_rate.load(), block_size.load(), channels.load()};
}

void SetAudioClock(AudioClock clock)
{
    if(mcu::FirmwareStarted())
        Fail("choose the audio clock before starting the firmware");
    clock_source.store(clock);
}

bool ProcessAudio(const float* const* in, float* const* out, size_t frames)
{
    if(clock_source.load() != AudioClock::kHost)
        Fail("ProcessAudio needs SetAudioClock(AudioClock::kHost)");
    const size_t bs = block_size.load();
    if(!started.load() || !mcu::FirmwareRunning() || bs == 0)
    {
        Silence(out, 0, frames);
        return false;
    }

    WriteFrames(in, frames);
    host_frames += frames;
    requested.store(host_frames / bs);
    mcu::Raise(mcu::Line::kAudio);

    // Wait for the output, at most twice the buffer's duration plus a little.
    const double   seconds = 2.0 * frames / sample_rate.load() + 0.005;
    timespec       deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    const uint64_t ns = uint64_t(deadline.tv_nsec) + uint64_t(seconds * 1e9);
    deadline.tv_sec += time_t(ns / 1000000000u);
    deadline.tv_nsec = long(ns % 1000000000u);
    while(out_ring.Readable() < frames * kChannels)
        if(sem_clockwait(&block_done.sem, CLOCK_MONOTONIC, &deadline) != 0 && errno == ETIMEDOUT)
            break;

    const size_t got = ReadFrames(out, frames);
    Silence(out, got, frames);
    return got == frames;
}

size_t WriteAudio(const float* const* in, size_t frames)
{
    return WriteFrames(in, frames);
}

size_t ReadAudio(float* const* out, size_t frames)
{
    return ReadFrames(out, frames);
}

AudioStats GetAudioStats()
{
    return {blocks.load(), underruns.load(), overruns.load()};
}

} // namespace daisycola
