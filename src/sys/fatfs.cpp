#include "fatfs.h"
#include "stub.h"

using namespace daisy;

FatFSInterface::Result FatFSInterface::Init(const uint8_t media) { DAISYCOLA_STUB(); }

extern "C" DWORD get_fattime(void) { return 0; }
