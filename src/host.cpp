#include "daisycola/host.h"

#include "mcu/vmcu.h"
#include "stub.h"

namespace daisycola
{
void Start(FirmwareMain firmware_main)
{
    DAISYCOLA_STUB();
}

void UseManualClock(bool manual)
{
    mcu::UseManualClock(manual);
}

void AdvanceClock(uint64_t microseconds)
{
    mcu::AdvanceClock(microseconds * 1000);
}

size_t ServiceInterrupts()
{
    return mcu::ServiceInterrupts();
}

} // namespace daisycola
