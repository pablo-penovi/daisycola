// I2CHandle backed by host device models (daisycola::I2CDevice).
//
// Blocking transfers take their bus time and call the device at once. DMA receives behave as in
// libDaisy: they start, and the DMA interrupt calls the firmware's callback when the transfer
// would be done. libDaisy shares one DMA stream between all I2C peripherals, so does the model.
#include "per/i2c.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "daisycola/host.h"
#include "mcu/vmcu.h"

using namespace daisy;

class I2CHandle::Impl
{
  public:
    Config config;
};

namespace
{
I2CHandle::Impl impls[4];

std::atomic<daisycola::I2CDevice*> devices[4][128];

struct DmaJob
{
    I2CHandle::Impl*               impl     = nullptr;
    uint8_t                        address  = 0;
    uint8_t*                       data     = nullptr;
    uint16_t                       size     = 0;
    I2CHandle::CallbackFunctionPtr callback = nullptr;
    void*                          context  = nullptr;
};

DmaJob            job; // touched only on the MCU thread
std::atomic<bool> dma_busy{false};

// The HAL shifts the address left by one and the peripheral sends bits 7:1, so only the low seven
// bits of what the firmware passes reach the bus.
uint8_t BusAddress(uint16_t address)
{
    return address & 0x7f;
}

daisycola::I2CDevice* Find(const I2CHandle::Impl& impl, uint16_t address)
{
    return devices[int(impl.config.periph)][BusAddress(address)].load();
}

// Address byte plus data, 9 clocks per byte.
uint64_t TransferNs(const I2CHandle::Impl& impl, uint16_t bytes)
{
    uint64_t hz = 100000;
    if(impl.config.speed == I2CHandle::Config::Speed::I2C_400KHZ)
        hz = 400000;
    else if(impl.config.speed == I2CHandle::Config::Speed::I2C_1MHZ)
        hz = 1000000;
    return (uint64_t(bytes) + 1) * 9 * 1000000000ull / hz;
}

void DmaComplete(void*)
{
    const DmaJob done = job;
    job               = DmaJob{};
    daisycola::I2CDevice* dev = Find(*done.impl, done.address);
    const bool            ok  = dev && dev->Read(done.data, done.size);
    dma_busy.store(false);
    if(done.callback)
        done.callback(done.context, ok ? I2CHandle::Result::OK : I2CHandle::Result::ERR);
}
} // namespace

I2CHandle::Result I2CHandle::Init(const Config& config)
{
    Impl& impl  = impls[int(config.periph)];
    impl.config = config;
    pimpl_      = &impl;
    daisycola::mcu::SetHandler(daisycola::mcu::Line::kI2c, DmaComplete, nullptr);
    daisycola::mcu::SetIrqNumber(daisycola::mcu::Line::kI2c, DMA1_Stream6_IRQn);
    return Result::OK;
}

const I2CHandle::Config& I2CHandle::GetConfig() const
{
    return pimpl_->config;
}

I2CHandle::Result
I2CHandle::TransmitBlocking(uint16_t address, uint8_t* data, uint16_t size, uint32_t timeout)
{
    daisycola::mcu::SleepUntil(daisycola::mcu::NowNs() + TransferNs(*pimpl_, size));
    daisycola::I2CDevice* dev = Find(*pimpl_, address);
    return dev && dev->Write(data, size) ? Result::OK : Result::ERR;
}

I2CHandle::Result I2CHandle::ReceiveDma(uint16_t            address,
                                        uint8_t*            data,
                                        uint16_t            size,
                                        CallbackFunctionPtr callback,
                                        void*               callback_context)
{
    if(pimpl_->config.periph == Config::Peripheral::I2C_4)
        return Result::ERR; // I2C4 has no DMA in libDaisy

    // libDaisy queues the job and waits for the running one to finish.
    while(dma_busy.load())
    {
        if(daisycola::mcu::InInterrupt())
        {
            std::fprintf(stderr, "daisycola: I2C DMA started from an interrupt while busy\n");
            std::abort();
        }
        daisycola::mcu::SleepUntil(daisycola::mcu::NowNs() + 10000);
    }
    dma_busy.store(true);
    job = {pimpl_, uint8_t(BusAddress(address)), data, size, callback, callback_context};
    daisycola::mcu::Schedule(daisycola::mcu::Line::kI2c, TransferNs(*pimpl_, size));
    return Result::OK;
}

namespace daisycola
{
void AttachI2CDevice(int bus, uint8_t address, I2CDevice* device)
{
    if(bus < 0 || bus > 3 || address > 0x7f)
    {
        std::fprintf(stderr, "daisycola: bad I2C bus or address\n");
        std::abort();
    }
    devices[bus][address].store(device);
}
} // namespace daisycola
