// MidiUsbTransport over the host's USB MIDI byte streams.
//
// The host's bytes raise the USB interrupt, whose handler passes them to the parser as libDaisy's
// receive callback does. Sends succeed while the host has USB connected; otherwise they fail after
// the fork's retries.
#include "hid/usb_midi.h"

#include <atomic>

#include "board/midi.h"
#include "mcu/vmcu.h"

using namespace daisy;

class MidiUsbTransport::Impl
{
  public:
    Config              config;
    std::atomic<bool>   rx_active{false};
    MidiRxParseCallback callback = nullptr;
    void*               context  = nullptr;

    static void Interrupt(void* context)
    {
        Impl& u = *static_cast<Impl*>(context);
        uint8_t buf[64];
        while(u.rx_active.load())
        {
            const size_t n = daisycola::midi::ReadIn(daisycola::MidiPort::kUsb, buf, sizeof buf);
            if(n == 0)
                return;
            if(u.callback)
                u.callback(buf, n, u.context);
        }
    }
};

namespace
{
MidiUsbTransport::Impl usb;
} // namespace

void MidiUsbTransport::Init(Config config)
{
    usb.config = config;
    usb.rx_active.store(false);
    pimpl_ = &usb;
    daisycola::mcu::SetHandler(daisycola::mcu::Line::kUsb, Impl::Interrupt, &usb);
    daisycola::mcu::SetIrqNumber(daisycola::mcu::Line::kUsb,
                                 config.periph == Config::EXTERNAL ? OTG_HS_IRQn : OTG_FS_IRQn);
    System::Delay(10); // as the fork's Init
}

// The fork resets the USB device; there's nothing to reset here.
void MidiUsbTransport::Reset() {}

void MidiUsbTransport::StartRx(MidiRxParseCallback callback, void* context)
{
    pimpl_->callback = callback;
    pimpl_->context  = context;
    pimpl_->rx_active.store(true);
    daisycola::mcu::Raise(daisycola::mcu::Line::kUsb); // bytes the host sent before
}

bool MidiUsbTransport::RxActive()
{
    return pimpl_->rx_active.load();
}

// Received bytes go straight to the parser, so there is no buffer to flush.
void MidiUsbTransport::FlushRx() {}

bool MidiUsbTransport::Tx(uint8_t* buffer, size_t size)
{
    for(int attempts = pimpl_->config.tx_retry_count;; attempts--)
    {
        if(daisycola::midi::UsbConnected())
        {
            daisycola::midi::WriteOut(daisycola::MidiPort::kUsb, buffer, size);
            return true;
        }
        if(attempts <= 0)
            return false;
        System::DelayUs(100);
    }
}
