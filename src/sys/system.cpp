#include "sys/system.h"
#include "stub.h"

using namespace daisy;

uint32_t System::GetNow() { DAISYCOLA_STUB(); }
void System::Delay(uint32_t delay_ms) { DAISYCOLA_STUB(); }
void System::DelayUs(uint32_t delay_us) { DAISYCOLA_STUB(); }
void System::DelayTicks(uint32_t delay_ticks) { DAISYCOLA_STUB(); }
uint32_t System::GetPClk2Freq() { DAISYCOLA_STUB(); }
