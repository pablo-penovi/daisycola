#include "per/tim_channel.h"
#include "stub.h"

using namespace daisy;

void TimChannel::Init(const Config& cfg) { DAISYCOLA_STUB(); }
void TimChannel::Start() { DAISYCOLA_STUB(); }
void TimChannel::SetPwm(uint32_t val) { DAISYCOLA_STUB(); }
void TimChannel::StartDma(void* data, size_t size, EndTransmissionFunctionPtr callback, void* cb_context) { DAISYCOLA_STUB(); }
const TimChannel::Config& TimChannel::GetConfig() const { DAISYCOLA_STUB(); }
