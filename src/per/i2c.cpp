#include "per/i2c.h"
#include "stub.h"

using namespace daisy;

I2CHandle::Result I2CHandle::Init(const Config& config) { DAISYCOLA_STUB(); }
I2CHandle::Result I2CHandle::TransmitBlocking(uint16_t address, uint8_t* data, uint16_t size, uint32_t timeout) { DAISYCOLA_STUB(); }
I2CHandle::Result I2CHandle::ReceiveDma(uint16_t address, uint8_t* data, uint16_t size, CallbackFunctionPtr callback, void* callback_context) { DAISYCOLA_STUB(); }
