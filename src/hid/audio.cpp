#include "hid/audio.h"
#include "stub.h"

using namespace daisy;

AudioHandle::Result AudioHandle::Init(const Config& config, SaiHandle sai) { DAISYCOLA_STUB(); }
AudioHandle::Result AudioHandle::Init(const Config& config, SaiHandle sai1, SaiHandle sai2) { DAISYCOLA_STUB(); }
float AudioHandle::GetSampleRate() { DAISYCOLA_STUB(); }
const AudioHandle::Config& AudioHandle::GetConfig() const { DAISYCOLA_STUB(); }
AudioHandle::Result AudioHandle::Start(AudioCallback callback) { DAISYCOLA_STUB(); }
