#include "per/tim.h"
#include "stub.h"

using namespace daisy;

TimerHandle::Result TimerHandle::Init(const Config& config) { DAISYCOLA_STUB(); }
TimerHandle::Result TimerHandle::SetPeriod(uint32_t ticks) { DAISYCOLA_STUB(); }
TimerHandle::Result TimerHandle::SetPrescaler(uint32_t val) { DAISYCOLA_STUB(); }
TimerHandle::Result TimerHandle::Start() { DAISYCOLA_STUB(); }
void TimerHandle::SetCallback(PeriodElapsedCallback cb, void* data) { DAISYCOLA_STUB(); }
