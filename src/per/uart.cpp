// UartHandler, as CHOMPI's MIDI uses it: USART1 at 31250 baud, receiving into a circular DMA
// buffer and sending with blocking writes.
//
// Bytes the host sends raise the UART interrupt. Its handler copies them into the firmware's
// circular buffer and calls the listener, as libDaisy's idle-line handler does.
#include "per/uart.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "board/midi.h"
#include "mcu/vmcu.h"

using namespace daisy;

class UartHandler::Impl
{
  public:
    Config                        config;
    uint8_t*                      buffer   = nullptr;
    size_t                        size     = 0;
    size_t                        position = 0;
    CircularRxCallbackFunctionPtr callback = nullptr;
    void*                         context  = nullptr;
    std::atomic<bool>             listening{false};

    static void Interrupt(void* context)
    {
        Impl& u = *static_cast<Impl*>(context);
        if(!u.listening.load())
            return;
        for(;;)
        {
            const size_t n = daisycola::midi::ReadIn(
                daisycola::MidiPort::kUart, u.buffer + u.position, u.size - u.position);
            if(n == 0)
                return;
            if(u.callback)
                u.callback(u.buffer + u.position, n, u.context, Result::OK);
            u.position = (u.position + n) % u.size;
        }
    }
};

namespace
{
UartHandler::Impl usart1;
} // namespace

UartHandler::Result UartHandler::Init(const Config& config)
{
    if(config.periph != Config::Peripheral::USART_1)
    {
        std::fprintf(stderr, "daisycola: only USART1 is modelled\n");
        std::abort();
    }
    usart1.listening.store(false);
    usart1.config = config;
    pimpl_        = &usart1;
    daisycola::mcu::SetHandler(daisycola::mcu::Line::kUart, Impl::Interrupt, &usart1);
    daisycola::mcu::SetIrqNumber(daisycola::mcu::Line::kUart, USART1_IRQn);
    return Result::OK;
}

UartHandler::Result UartHandler::DmaListenStart(uint8_t*                      buff,
                                                size_t                        size,
                                                CircularRxCallbackFunctionPtr cb,
                                                void*                         callback_context)
{
    if(size == 0)
        return Result::ERR;
    pimpl_->buffer   = buff;
    pimpl_->size     = size;
    pimpl_->position = 0;
    pimpl_->callback = cb;
    pimpl_->context  = callback_context;
    pimpl_->listening.store(true);
    daisycola::mcu::Raise(daisycola::mcu::Line::kUart); // bytes the host sent before
    return Result::OK;
}

bool UartHandler::IsListening() const
{
    return pimpl_->listening.load();
}

// A blocking write takes the bytes' time on the wire: 10 bits each with 8N1.
UartHandler::Result UartHandler::PollTx(uint8_t* buff, size_t size)
{
    const uint64_t baud = pimpl_->config.baudrate ? pimpl_->config.baudrate : 31250;
    daisycola::mcu::SleepUntil(daisycola::mcu::NowNs() + size * 10 * 1000000000ull / baud);
    daisycola::midi::WriteOut(daisycola::MidiPort::kUart, buff, size);
    return Result::OK;
}
