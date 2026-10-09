// SaiHandle: only the configuration. The audio data path is AudioHandle's (hid/audio.cpp).
#include "per/sai.h"

using namespace daisy;

class SaiHandle::Impl
{
  public:
    Config config;
};

namespace
{
SaiHandle::Impl impls[2];
} // namespace

SaiHandle::Result SaiHandle::Init(const Config& config)
{
    const int idx = int(config.periph);
    if(idx < 0 || idx >= 2)
        return Result::ERR;
    impls[idx].config = config;
    pimpl_            = &impls[idx];
    return Result::OK;
}

const SaiHandle::Config& SaiHandle::GetConfig() const
{
    return pimpl_->config;
}

float SaiHandle::GetSampleRate()
{
    switch(pimpl_->config.sr)
    {
        case Config::SampleRate::SAI_8KHZ: return 8000.f;
        case Config::SampleRate::SAI_16KHZ: return 16000.f;
        case Config::SampleRate::SAI_32KHZ: return 32000.f;
        case Config::SampleRate::SAI_48KHZ: return 48000.f;
        case Config::SampleRate::SAI_96KHZ: return 96000.f;
    }
    return 48000.f;
}
