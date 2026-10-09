// Phase 2: the virtual SD card.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

#include "daisycola/host.h"
#include "diskio.h"
#include "fatfs.h"
#include "ff_gen_drv.h"
#include "test_util.h"

namespace fs = std::filesystem;
using namespace daisycola;

namespace
{
std::vector<uint8_t> ReadHostFile(const fs::path& path)
{
    std::vector<uint8_t> data(fs::file_size(path));
    std::ifstream(path, std::ios::binary).read(reinterpret_cast<char*>(data.data()), data.size());
    return data;
}

void WriteHostFile(const fs::path& path, const std::string& text)
{
    std::ofstream(path, std::ios::binary) << text;
}

constexpr uint64_t kMiB = 1024 * 1024;

} // namespace

TEST(SdCard, FormatsAnEmptyFat32Image)
{
    TempDir dir;
    SdCreateImage(dir / "card.img", 256 * kMiB);
    EXPECT_EQ(fs::file_size(dir / "card.img"), 256 * kMiB);

    SdOpenImage(dir / "card.img");
    EXPECT_TRUE(SdList("/").empty());

    // The volume is FAT32 and sits in an MBR partition, like a real card.
    std::ifstream img(dir / "card.img", std::ios::binary);
    uint8_t       mbr[512];
    img.read(reinterpret_cast<char*>(mbr), sizeof(mbr));
    EXPECT_EQ(mbr[510], 0x55);
    EXPECT_EQ(mbr[511], 0xAA);
    EXPECT_EQ(mbr[446 + 4], 0x0C); // partition type: FAT32 with LBA
    SdCloseImage();
}

TEST(SdCard, CopiesNestedDirectoriesInAndOut)
{
    TempDir dir;
    fs::create_directories(dir.path() / "src/chromatic/deep");
    WriteHostFile(dir.path() / "src/options.json", "{\"midi_ch_in\": 1}");
    WriteHostFile(dir.path() / "src/chromatic/a long file name.wav", std::string(100000, 'x'));
    WriteHostFile(dir.path() / "src/chromatic/deep/z.bin", "zz");

    SdCreateImage(dir / "card.img", 128 * kMiB);
    SdOpenImage(dir / "card.img");
    SdCopyIn(dir / "src", "/");

    auto root = SdList("/");
    std::sort(root.begin(), root.end(), [](auto& a, auto& b) { return a.name < b.name; });
    ASSERT_EQ(root.size(), 2u);
    EXPECT_EQ(root[0].name, "chromatic");
    EXPECT_TRUE(root[0].is_dir);
    EXPECT_EQ(root[1].name, "options.json");
    EXPECT_EQ(root[1].size, 17u);

    const auto wav = SdReadFile("/chromatic/a long file name.wav");
    EXPECT_EQ(wav.size(), 100000u);

    SdCopyOut("/", dir / "out");
    EXPECT_EQ(ReadHostFile(dir.path() / "out/chromatic/deep/z.bin"),
              ReadHostFile(dir.path() / "src/chromatic/deep/z.bin"));
    EXPECT_EQ(ReadHostFile(dir.path() / "out/chromatic/a long file name.wav"),
              ReadHostFile(dir.path() / "src/chromatic/a long file name.wav"));
    SdCloseImage();
}

// The path the firmware takes: FatFSInterface, f_mount and plain FatFs calls.
TEST(SdCard, FirmwareSeesTheCardThroughFatFSInterface)
{
    TempDir dir;
    WriteHostFile(dir.path() / "presets.json", "[1,2,3]");
    SdCreateImage(dir / "card.img", 64 * kMiB);
    SdOpenImage(dir / "card.img");
    SdCopyIn(dir / "presets.json", "/");

    daisy::FatFSInterface fsi;
    ASSERT_EQ(fsi.Init(daisy::FatFSInterface::Config::MEDIA_SD), daisy::FatFSInterface::OK);
    ASSERT_EQ(f_mount(&fsi.GetSDFileSystem(), fsi.GetSDPath(), 1), FR_OK);

    FIL fil;
    ASSERT_EQ(f_open(&fil, "presets.json", FA_OPEN_ALWAYS | FA_WRITE | FA_READ), FR_OK);
    EXPECT_EQ(f_size(&fil), 7u);
    char buf[16] = {};
    UINT br      = 0;
    ASSERT_EQ(f_read(&fil, buf, sizeof(buf), &br), FR_OK);
    EXPECT_STREQ(buf, "[1,2,3]");

    // Rewrite it the way TAPE's preset code does.
    ASSERT_EQ(f_lseek(&fil, 0), FR_OK);
    ASSERT_EQ(f_write(&fil, "[9]", 3, nullptr), FR_OK);
    ASSERT_EQ(f_truncate(&fil), FR_OK);
    ASSERT_EQ(f_sync(&fil), FR_OK);
    ASSERT_EQ(f_close(&fil), FR_OK);

    // Pulling the card shows up in disk_status, which TAPE polls.
    EXPECT_EQ(disk_status(0), 0);
    SdSetPresent(false);
    EXPECT_NE(disk_status(0), 0);
    SdSetPresent(true);

    f_mount(nullptr, fsi.GetSDPath(), 0);
    FATFS_UnLinkDriver(const_cast<char*>(fsi.GetSDPath()));

    const auto data = SdReadFile("/presets.json");
    EXPECT_EQ(std::string(data.begin(), data.end()), "[9]");
    SdCloseImage();
}

TEST(SdCard, HoldsTheTapeFactoryCard)
{
    const fs::path card = DAISYCOLA_TAPE_CARD_DIR;
    if(card.empty())
        GTEST_SKIP() << "CHOMPI not available";

    TempDir dir;
    SdCreateImage(dir / "card.img", 512 * kMiB);
    SdOpenImage(dir / "card.img");
    SdCopyIn(card.string(), "/");

    std::vector<std::string> expected;
    for(const auto& e : fs::directory_iterator(card))
        expected.push_back(e.path().filename().string());
    std::sort(expected.begin(), expected.end());

    std::vector<std::string> listed;
    for(const auto& e : SdList("/"))
    {
        listed.push_back(e.name);
        EXPECT_EQ(e.size, fs::file_size(card / e.name)) << e.name;
    }
    std::sort(listed.begin(), listed.end());
    EXPECT_EQ(listed, expected);

    SdCopyOut("/", dir / "out");
    for(const auto& name : expected)
        EXPECT_TRUE(ReadHostFile(dir.path() / "out" / name) == ReadHostFile(card / name)) << name;
    SdCloseImage();
}
