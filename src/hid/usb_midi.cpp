#include "hid/usb_midi.h"
#include "stub.h"

using namespace daisy;

void MidiUsbTransport::Init(Config config) { DAISYCOLA_STUB(); }
void MidiUsbTransport::Reset() { DAISYCOLA_STUB(); }
void MidiUsbTransport::StartRx(MidiRxParseCallback callback, void* context) { DAISYCOLA_STUB(); }
bool MidiUsbTransport::RxActive() { DAISYCOLA_STUB(); }
void MidiUsbTransport::FlushRx() { DAISYCOLA_STUB(); }
bool MidiUsbTransport::Tx(uint8_t* buffer, size_t size) { DAISYCOLA_STUB(); }
