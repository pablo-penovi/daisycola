#include "per/uart.h"
#include "stub.h"

using namespace daisy;

UartHandler::Result UartHandler::Init(const Config& config) { DAISYCOLA_STUB(); }
UartHandler::Result UartHandler::DmaListenStart(uint8_t* buff, size_t size, CircularRxCallbackFunctionPtr cb, void* callback_context) { DAISYCOLA_STUB(); }
bool UartHandler::IsListening() const { DAISYCOLA_STUB(); }
UartHandler::Result UartHandler::PollTx(uint8_t* buff, size_t size) { DAISYCOLA_STUB(); }
