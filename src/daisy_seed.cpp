#include "daisy_seed.h"

using namespace daisy;

// As libDaisy's DaisySeed::Init, for the parts daisycola models: the clocks and the on-board
// codec on SAI1 (48 kHz, 24 bit, 48-frame blocks).
void DaisySeed::Init(bool boost)
{
    System::Config syscfg;
    if(boost)
        syscfg.Boost();
    else
        syscfg.Defaults();
    static System system;
    system.Init(syscfg);

    SaiHandle::Config sai_config;
    sai_config.periph    = SaiHandle::Config::Peripheral::SAI_1;
    sai_config.sr        = SaiHandle::Config::SampleRate::SAI_48KHZ;
    sai_config.bit_depth = SaiHandle::Config::BitDepth::SAI_24BIT;
    sai_config.a_sync    = SaiHandle::Config::Sync::MASTER;
    sai_config.b_sync    = SaiHandle::Config::Sync::SLAVE;
    sai_config.a_dir     = SaiHandle::Config::Direction::RECEIVE;
    sai_config.b_dir     = SaiHandle::Config::Direction::TRANSMIT;
    sai_1_handle_.Init(sai_config);

    AudioHandle::Config audio_config;
    audio_config.blocksize  = 48;
    audio_config.samplerate = SaiHandle::Config::SampleRate::SAI_48KHZ;
    audio_config.postgain   = 1.f;
    audio_handle.Init(audio_config, sai_1_handle_);
}

void DaisySeed::StartAudio(AudioHandle::AudioCallback cb)
{
    audio_handle.Start(cb);
}

float DaisySeed::AudioSampleRate()
{
    return audio_handle.GetSampleRate();
}

size_t DaisySeed::AudioBlockSize()
{
    return audio_handle.GetConfig().blocksize;
}

const SaiHandle& DaisySeed::AudioSaiHandle() const
{
    return sai_1_handle_;
}
