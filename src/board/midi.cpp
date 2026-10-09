#include "board/midi.h"

#include <atomic>
#include <mutex>

#include "mcu/vmcu.h"
#include "util/spsc_ring.h"

namespace daisycola
{
namespace
{
constexpr size_t kRingBytes = 4096;

struct Port
{
    SpscRing<uint8_t, kRingBytes> in;  // host -> receive interrupt
    SpscRing<uint8_t, kRingBytes> out; // firmware -> host
    std::mutex                    host_in, host_out; // host threads take turns on their ends
    mcu::Line                     line;
};

Port ports[2];

std::atomic<bool> usb_connected{false};

Port& Get(MidiPort port)
{
    return ports[int(port)];
}

struct InitLines
{
    InitLines()
    {
        ports[int(MidiPort::kUart)].line = mcu::Line::kUart;
        ports[int(MidiPort::kUsb)].line  = mcu::Line::kUsb;
    }
} init_lines;
} // namespace

size_t midi::ReadIn(MidiPort port, uint8_t* data, size_t size)
{
    return Get(port).in.Read(data, size);
}

void midi::WriteOut(MidiPort port, const uint8_t* data, size_t size)
{
    // The firmware sends from main and from interrupts: mask them so the writes don't interleave.
    mcu::Critical critical;
    Port&         p = Get(port);
    if(p.out.Writable() >= size)
        p.out.Write(data, size);
}

bool midi::UsbConnected()
{
    return usb_connected.load();
}

size_t WriteMidiIn(MidiPort port, const uint8_t* data, size_t size)
{
    if(port == MidiPort::kUsb && !usb_connected.load())
        return 0;
    Port& p = Get(port);
    size_t n;
    {
        std::lock_guard<std::mutex> lock(p.host_in);
        n = p.in.Write(data, size);
    }
    if(n > 0)
        mcu::Raise(p.line);
    return n;
}

size_t ReadMidiOut(MidiPort port, uint8_t* data, size_t size)
{
    Port&                       p = Get(port);
    std::lock_guard<std::mutex> lock(p.host_out);
    return p.out.Read(data, size);
}

void SetUsbConnected(bool connected)
{
    usb_connected.store(connected);
}

} // namespace daisycola
