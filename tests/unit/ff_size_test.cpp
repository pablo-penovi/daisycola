// include/ff.h: f_size() evaluates TAPE's sample-point arithmetic the way the STM32 does.
#include <gtest/gtest.h>
#include <type_traits>
#include <vector>

#include "fatfs.h"

namespace
{
// Stands in for TAPE's WAV_FormatTypeDef: TAPE subtracts its sizeof, a size_t.
struct WavHeader
{
    uint8_t bytes[44];
};

FIL FileOfSize(uint32_t size)
{
    FIL fil{};
    fil.obj.objsize = size;
    return fil;
}

// The expressions TAPE uses (SampleReader.h SetStartPoint/SetEndPoint, GetStartPoint/GetEndPoint).
uint32_t Point(float val, FIL& fil)
{
    return val * (f_size(&fil) - sizeof(WavHeader));
}
uint32_t PointPlusHeader(float val, FIL& fil)
{
    return val * (f_size(&fil) - sizeof(WavHeader)) + sizeof(WavHeader);
}
} // namespace

TEST(FfSize, NoFileOpenSaturatesLikeTheChip)
{
    // 0u - 44u = 4294967252, which rounds to 2^32 as a float. vcvt saturates to 0xFFFFFFFF.
    FIL fil = FileOfSize(0);
    EXPECT_EQ(Point(1.f, fil), 0xFFFFFFFFu);
    EXPECT_EQ(PointPlusHeader(1.f, fil), 0xFFFFFFFFu);
    // .5f * 2^32 is exactly 2^31: the float rounding happens before the multiply.
    EXPECT_EQ(Point(.5f, fil), 2147483648u);
    EXPECT_EQ(Point(.25f, fil), 1073741824u);
    EXPECT_EQ(Point(0.f, fil), 0u);
    EXPECT_EQ(Point(-1.f, fil), 0u);
}

TEST(FfSize, OpenFileIsUnchanged)
{
    FIL fil = FileOfSize(1843244);
    EXPECT_EQ(f_size(&fil), 1843244u);
    EXPECT_EQ(Point(1.f, fil), 1843200u);
    EXPECT_EQ(Point(.5f, fil), 921600u);
    EXPECT_EQ(PointPlusHeader(1.f, fil), 1843244u);
    EXPECT_EQ(PointPlusHeader(.5f, fil), 921644u);
}

TEST(FfSize, IntegerArithmeticWrapsAt32Bits)
{
    FIL empty = FileOfSize(0);
    EXPECT_EQ(uint32_t(f_size(&empty) - sizeof(WavHeader)), 4294967252u);
    EXPECT_EQ(uint64_t(f_size(&empty) - sizeof(WavHeader)), 4294967252u);

    FIL fil  = FileOfSize(1000);
    fil.fptr = 200;
    EXPECT_EQ(f_size(&fil) - f_tell(&fil), 800u); // FileStreamingManager.cpp, SampleReader.h

    int pos = -100; // SampleReader.h: pos += (f_size - sizeof)
    pos += (f_size(&fil) - sizeof(WavHeader));
    EXPECT_EQ(pos, 856);

    // SampleReader.h: `f_size - sizeof % 8 != 0` is `(f_size - 4) != 0`.
    EXPECT_TRUE(f_size(&fil) - sizeof(WavHeader) % 8 != 0);
    EXPECT_TRUE(f_size(&fil) != 0);
    EXPECT_FALSE(f_size(&empty) != 0);
    EXPECT_TRUE(f_tell(&fil) < f_size(&fil));

    std::vector<uint8_t> data(f_size(&fil)); // src/board/sd_card.cpp
    EXPECT_EQ(data.size(), 1000u);
}

// A product converts to uint32_t only, so a use the wrapper doesn't model can't silently fall
// back to x86 semantics.
using Product = decltype(1.f * (f_size(static_cast<FIL*>(nullptr)) - sizeof(WavHeader)));
static_assert(std::is_convertible_v<Product, uint32_t>);
static_assert(!std::is_convertible_v<Product, int>);
static_assert(!std::is_convertible_v<Product, uint64_t>);
static_assert(!std::is_convertible_v<Product, float>);
static_assert(!std::is_convertible_v<Product, bool>);
