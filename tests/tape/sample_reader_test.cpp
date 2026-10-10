// TAPE's FileSampleReader accepts start and end points before its file is open, as on the chip.
//
// A cubbi's first press sets the play window while the voice's file is still being opened, so
// f_size() is 0. On the STM32 the points still come out in range (see include/ff.h) and are
// accepted; if they're rejected, the voice plays an empty window and the press is silent.
#include <gtest/gtest.h>
#include <vector>

#include "SampleReader.h"

using namespace daisy;

namespace
{
// TAPE's readers and manager are globals, so they start zeroed: no file open.
FileStreamingManager manager;
FileSampleReader     reader;
} // namespace

TEST(TapeSampleReader, AcceptsPointsBeforeTheFileOpens)
{
    std::vector<int16_t> mem(kMaxRamBuffSize);
    RamBufferMemory      ram;
    ram.Init(mem.data());

    reader.Init(manager, 48000.f, &ram); // RestoreDefaults: SetStartPoint(0), SetEndPoint(1)

    // What DSPEngine's StartPlayback does for a cubbi, before the OPEN request is served.
    EXPECT_TRUE(reader.SetStartPoint(.25f));
    EXPECT_TRUE(reader.SetEndPoint(.75f));
}
