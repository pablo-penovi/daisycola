#include "daisycola/host.h"

#include "mcu/vmcu.h"

namespace daisycola
{
static_assert(int(Irq::kTim5) == int(mcu::Line::kTim5) && int(Irq::kUsb) == int(mcu::Line::kUsb),
              "Irq follows mcu::Line");

void Start(FirmwareMain firmware_main)
{
    mcu::StartFirmware(firmware_main);
}

bool Halt(uint32_t timeout_ms)
{
    return mcu::HaltFirmware(timeout_ms);
}

void Wake()
{
    mcu::Wake();
}

BoardState GetBoardState()
{
    const mcu::FirmwareState s = mcu::GetFirmwareState();
    return {s.started, s.running, s.sleeping, s.exited, s.exit_code, s.sleeps};
}

IrqStats GetIrqStats(Irq irq)
{
    const mcu::LineStats s = mcu::GetLineStats(mcu::Line(int(irq)));
    return {s.count, s.total_ns, s.max_ns, s.dropped};
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
