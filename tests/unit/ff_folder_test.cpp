// The SD card as a host folder: FatFs's API on the files in it (src/sys/ff_folder.cpp).
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "daisycola/host.h"
#include "fatfs.h"
#include "test_util.h"

namespace fs = std::filesystem;
using namespace daisycola;

namespace
{
// A card folder, inserted and mounted the way firmware does it: FatFSInterface, then f_mount.
class Card : public ::testing::Test
{
  protected:
    TempDir               dir;
    daisy::FatFSInterface fsi;

    void SetUp() override
    {
        SdInsert(dir.path());
        ASSERT_EQ(fsi.Init(daisy::FatFSInterface::Config::MEDIA_SD), daisy::FatFSInterface::OK);
        ASSERT_STREQ(fsi.GetSDPath(), "0:/");
        ASSERT_EQ(f_mount(&fsi.GetSDFileSystem(), fsi.GetSDPath(), 1), FR_OK);
    }

    void TearDown() override
    {
        f_mount(nullptr, "0:", 0);
        SdSetPresent(true);
        SdEject();
    }

    fs::path Host(const std::string& name) const { return dir.path() / name; }

    void Put(const std::string& name, const std::string& text) const
    {
        std::ofstream(Host(name), std::ios::binary) << text;
    }

    std::string Get(const std::string& name) const
    {
        std::ifstream in(Host(name), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }

    std::vector<std::string> List(const char* path)
    {
        DIR                      d;
        FILINFO                  info;
        std::vector<std::string> names;
        EXPECT_EQ(f_opendir(&d, path), FR_OK);
        while(f_readdir(&d, &info) == FR_OK && info.fname[0])
            names.push_back(info.fname);
        EXPECT_EQ(f_closedir(&d), FR_OK);
        std::sort(names.begin(), names.end());
        return names;
    }
};
} // namespace

TEST_F(Card, OpensInEachMode)
{
    FIL f;
    EXPECT_EQ(f_open(&f, "a.txt", FA_READ), FR_NO_FILE) << "FA_OPEN_EXISTING";

    ASSERT_EQ(f_open(&f, "a.txt", FA_CREATE_NEW | FA_WRITE), FR_OK);
    ASSERT_EQ(f_write(&f, "hello", 5, nullptr), FR_OK);
    ASSERT_EQ(f_close(&f), FR_OK);
    EXPECT_EQ(f_open(&f, "a.txt", FA_CREATE_NEW | FA_WRITE), FR_EXIST);

    ASSERT_EQ(f_open(&f, "a.txt", FA_OPEN_ALWAYS | FA_READ), FR_OK) << "keeps the file";
    EXPECT_EQ(f_size(&f), 5u);
    EXPECT_EQ(f_tell(&f), 0u);
    ASSERT_EQ(f_close(&f), FR_OK);

    ASSERT_EQ(f_open(&f, "b.txt", FA_OPEN_ALWAYS | FA_WRITE), FR_OK) << "creates the file";
    ASSERT_EQ(f_close(&f), FR_OK);
    EXPECT_TRUE(fs::exists(Host("b.txt")));

    ASSERT_EQ(f_open(&f, "a.txt", FA_OPEN_APPEND | FA_WRITE), FR_OK);
    EXPECT_EQ(f_tell(&f), 5u) << "append starts at the end";
    ASSERT_EQ(f_write(&f, "!", 1, nullptr), FR_OK);
    ASSERT_EQ(f_close(&f), FR_OK);
    EXPECT_EQ(Get("a.txt"), "hello!");

    ASSERT_EQ(f_open(&f, "a.txt", FA_CREATE_ALWAYS | FA_WRITE), FR_OK);
    EXPECT_EQ(f_size(&f), 0u) << "create-always truncates";
    ASSERT_EQ(f_close(&f), FR_OK);
    EXPECT_EQ(Get("a.txt"), "");

    // Access follows the mode: no reading a write-only file, or writing a read-only one.
    Put("c.txt", "abc");
    char buf[4];
    UINT n = 9;
    ASSERT_EQ(f_open(&f, "c.txt", FA_WRITE), FR_OK);
    EXPECT_EQ(f_read(&f, buf, 3, &n), FR_DENIED);
    EXPECT_EQ(n, 0u);
    ASSERT_EQ(f_close(&f), FR_OK);
    ASSERT_EQ(f_open(&f, "c.txt", FA_READ), FR_OK);
    EXPECT_EQ(f_write(&f, "x", 1, &n), FR_DENIED);
    EXPECT_EQ(f_truncate(&f), FR_DENIED);
    ASSERT_EQ(f_close(&f), FR_OK);

    fs::create_directory(Host("dir"));
    EXPECT_EQ(f_open(&f, "dir", FA_READ), FR_NO_FILE);
    EXPECT_EQ(f_open(&f, "dir", FA_OPEN_ALWAYS | FA_WRITE), FR_DENIED);
    EXPECT_EQ(f_open(&f, "", FA_READ), FR_INVALID_NAME);
    EXPECT_EQ(f_open(&f, "missing/x.txt", FA_CREATE_ALWAYS | FA_WRITE), FR_NO_PATH);
    EXPECT_EQ(f_close(&f), FR_INVALID_OBJECT) << "a failed open leaves nothing to close";
}

TEST_F(Card, ReadsAndWritesKeepingTheFileFieldsCurrent)
{
    FIL f;
    ASSERT_EQ(f_open(&f, "0:/data.bin", FA_CREATE_ALWAYS | FA_WRITE | FA_READ), FR_OK);
    EXPECT_EQ(f.obj.fs, &fsi.GetSDFileSystem());
    UINT n = 0;
    ASSERT_EQ(f_write(&f, "0123456789", 10, &n), FR_OK);
    EXPECT_EQ(n, 10u);
    EXPECT_EQ(f_size(&f), 10u);
    EXPECT_EQ(f_tell(&f), 10u);
    EXPECT_TRUE(f_eof(&f));

    ASSERT_EQ(f_lseek(&f, 4), FR_OK);
    EXPECT_FALSE(f_eof(&f));
    char buf[16] = {};
    ASSERT_EQ(f_read(&f, buf, sizeof buf, &n), FR_OK);
    EXPECT_EQ(n, 6u) << "reads stop at the end";
    EXPECT_STREQ(buf, "456789");
    EXPECT_TRUE(f_eof(&f));

    ASSERT_EQ(f_rewind(&f), FR_OK);
    ASSERT_EQ(f_write(&f, "ab", 2, &n), FR_OK);
    EXPECT_EQ(f_size(&f), 10u) << "overwriting doesn't grow the file";
    ASSERT_EQ(f_sync(&f), FR_OK);
    EXPECT_EQ(Get("data.bin"), "ab23456789") << "the host sees writes at once";
    ASSERT_EQ(f_close(&f), FR_OK);
    EXPECT_EQ(f.obj.fs, nullptr);
    EXPECT_EQ(f_read(&f, buf, 1, &n), FR_INVALID_OBJECT);
}

// CHOMPI passes NULL when it doesn't need the count.
TEST_F(Card, WritesWithoutAByteCount)
{
    FIL f;
    ASSERT_EQ(f_open(&f, "n.txt", FA_CREATE_ALWAYS | FA_WRITE), FR_OK);
    EXPECT_EQ(f_write(&f, "abc", 3, nullptr), FR_OK);
    ASSERT_EQ(f_close(&f), FR_OK);
    EXPECT_EQ(Get("n.txt"), "abc");
}

TEST_F(Card, SeeksPastTheEnd)
{
    Put("s.bin", "abcd");
    FIL f;
    ASSERT_EQ(f_open(&f, "s.bin", FA_READ), FR_OK);
    ASSERT_EQ(f_lseek(&f, 100), FR_OK);
    EXPECT_EQ(f_tell(&f), 4u) << "a read-only file stops at its end";
    ASSERT_EQ(f_close(&f), FR_OK);

    ASSERT_EQ(f_open(&f, "s.bin", FA_READ | FA_WRITE), FR_OK);
    ASSERT_EQ(f_lseek(&f, 8), FR_OK);
    EXPECT_EQ(f_tell(&f), 8u);
    EXPECT_EQ(f_size(&f), 8u) << "a writable file grows";
    ASSERT_EQ(f_write(&f, "z", 1, nullptr), FR_OK);
    ASSERT_EQ(f_close(&f), FR_OK);
    EXPECT_EQ(Get("s.bin"), std::string("abcd\0\0\0\0z", 9));
}

TEST_F(Card, Truncates)
{
    Put("t.json", "[1,2,3,4,5]");
    FIL f;
    ASSERT_EQ(f_open(&f, "t.json", FA_OPEN_ALWAYS | FA_WRITE | FA_READ), FR_OK);
    ASSERT_EQ(f_write(&f, "[9]", 3, nullptr), FR_OK);
    ASSERT_EQ(f_truncate(&f), FR_OK);
    EXPECT_EQ(f_size(&f), 3u);
    ASSERT_EQ(f_close(&f), FR_OK);
    EXPECT_EQ(Get("t.json"), "[9]");
}

TEST_F(Card, RenamesWithoutReplacing)
{
    Put("from.json", "new");
    Put("to.json", "old");
    EXPECT_EQ(f_rename("from.json", "to.json"), FR_EXIST) << "FatFs never replaces a file";
    EXPECT_EQ(Get("to.json"), "old");

    // TAPE's way of replacing presets.json.
    ASSERT_EQ(f_unlink("to.json"), FR_OK);
    ASSERT_EQ(f_rename("from.json", "to.json"), FR_OK);
    EXPECT_EQ(Get("to.json"), "new");
    EXPECT_FALSE(fs::exists(Host("from.json")));

    EXPECT_EQ(f_rename("to.json", "TO.JSON"), FR_OK) << "a change of case is the same file";
    EXPECT_TRUE(fs::exists(Host("TO.JSON")));
    EXPECT_EQ(f_rename("missing", "x"), FR_NO_FILE);
    fs::create_directory(Host("sub"));
    ASSERT_EQ(f_rename("TO.JSON", "sub/moved.json"), FR_OK);
    EXPECT_EQ(Get("sub/moved.json"), "new");
    EXPECT_EQ(f_rename("sub/moved.json", "nowhere/x.json"), FR_NO_PATH);
}

TEST_F(Card, Unlinks)
{
    Put("f.txt", "x");
    fs::create_directories(Host("full/inner"));
    fs::create_directory(Host("empty"));
    EXPECT_EQ(f_unlink("f.txt"), FR_OK);
    EXPECT_FALSE(fs::exists(Host("f.txt")));
    EXPECT_EQ(f_unlink("f.txt"), FR_NO_FILE);
    EXPECT_EQ(f_unlink("full"), FR_DENIED) << "a directory must be empty";
    EXPECT_EQ(f_rmdir("empty"), FR_OK);
    EXPECT_FALSE(fs::exists(Host("empty")));

    Put("ro.txt", "x");
    fs::permissions(Host("ro.txt"), fs::perms::owner_read);
    EXPECT_EQ(f_unlink("ro.txt"), FR_DENIED) << "read-only, as AM_RDO";
}

TEST_F(Card, Stats)
{
    Put("sample.wav", std::string(1234, 's'));
    Put(".batt_log.txt", "");
    Put("ro.txt", "");
    fs::permissions(Host("ro.txt"), fs::perms::owner_read);
    fs::create_directory(Host("dir"));

    FILINFO info;
    ASSERT_EQ(f_stat("sample.wav", &info), FR_OK);
    EXPECT_EQ(info.fsize, 1234u);
    EXPECT_EQ(info.fattrib, 0);
    EXPECT_STREQ(info.fname, "sample.wav");
    EXPECT_EQ(info.fdate, 0);
    EXPECT_EQ(info.ftime, 0);
    ASSERT_EQ(f_stat("dir", &info), FR_OK);
    EXPECT_EQ(info.fattrib, AM_DIR);
    ASSERT_EQ(f_stat(".batt_log.txt", &info), FR_OK);
    EXPECT_EQ(info.fattrib, AM_HID);
    ASSERT_EQ(f_stat("ro.txt", &info), FR_OK);
    EXPECT_EQ(info.fattrib, AM_RDO);

    EXPECT_EQ(f_stat("sample.wav", nullptr), FR_OK) << "TAPE checks existence without a FILINFO";
    EXPECT_EQ(f_stat("missing.wav", nullptr), FR_NO_FILE);
    EXPECT_EQ(f_stat("missing/x.wav", nullptr), FR_NO_PATH);
    EXPECT_EQ(f_stat("/", &info), FR_INVALID_NAME) << "the root has no entry";
}

TEST_F(Card, ReadsDirectories)
{
    Put("b.wav", "");
    Put("a.json", "");
    fs::create_directory(Host("sub"));
    Put("sub/inner.txt", "");
    EXPECT_EQ(List("/"), (std::vector<std::string>{"a.json", "b.wav", "sub"})) << "no . or ..";
    EXPECT_EQ(List("sub"), (std::vector<std::string>{"inner.txt"}));

    DIR     d;
    FILINFO info;
    ASSERT_EQ(f_opendir(&d, ""), FR_OK);
    int n = 0;
    while(f_readdir(&d, &info) == FR_OK && info.fname[0])
        n++;
    ASSERT_EQ(f_rewinddir(&d), FR_OK);
    int again = 0;
    while(f_readdir(&d, &info) == FR_OK && info.fname[0])
        again++;
    EXPECT_EQ(again, n);
    ASSERT_EQ(f_closedir(&d), FR_OK);
    EXPECT_EQ(f_readdir(&d, &info), FR_INVALID_OBJECT);
    EXPECT_EQ(f_opendir(&d, "b.wav"), FR_NO_PATH);
    EXPECT_EQ(f_opendir(&d, "nothing"), FR_NO_PATH);

    std::vector<std::string> found;
    for(FRESULT r = f_findfirst(&d, &info, "/", "*.WAV"); r == FR_OK && info.fname[0];
        r      = f_findnext(&d, &info))
        found.push_back(info.fname);
    f_closedir(&d);
    EXPECT_EQ(found, (std::vector<std::string>{"b.wav"})) << "patterns ignore case";
}

TEST_F(Card, MakesAndChangesDirectories)
{
    ASSERT_EQ(f_mkdir("one"), FR_OK);
    EXPECT_TRUE(fs::is_directory(Host("one")));
    EXPECT_EQ(f_mkdir("one"), FR_EXIST);
    EXPECT_EQ(f_mkdir("ONE"), FR_EXIST) << "names ignore case";
    ASSERT_EQ(f_mkdir("one/two"), FR_OK);
    EXPECT_EQ(f_mkdir("x/y"), FR_NO_PATH);

    ASSERT_EQ(f_chdir("One/TWO"), FR_OK);
    char cwd[64];
    ASSERT_EQ(f_getcwd(cwd, sizeof cwd), FR_OK);
    EXPECT_STREQ(cwd, "0:/one/two") << "the real names";
    EXPECT_EQ(f_getcwd(cwd, 5), FR_NOT_ENOUGH_CORE);

    FIL f;
    ASSERT_EQ(f_open(&f, "here.txt", FA_CREATE_NEW | FA_WRITE), FR_OK);
    ASSERT_EQ(f_close(&f), FR_OK);
    EXPECT_TRUE(fs::exists(Host("one/two/here.txt"))) << "relative to the current directory";
    ASSERT_EQ(f_stat("../two/./here.txt", nullptr), FR_OK);
    ASSERT_EQ(f_stat("/one/two/here.txt", nullptr), FR_OK);
    EXPECT_EQ(f_unlink("/one/two"), FR_DENIED) << "the current directory stays";
    ASSERT_EQ(f_chdir("/"), FR_OK);
    ASSERT_EQ(f_getcwd(cwd, sizeof cwd), FR_OK);
    EXPECT_STREQ(cwd, "0:/");
    EXPECT_EQ(f_chdir("one/two/here.txt"), FR_NO_PATH);
    EXPECT_EQ(f_chdrive("0:"), FR_OK);
    EXPECT_EQ(f_chdrive("1:"), FR_INVALID_DRIVE);
}

TEST_F(Card, MatchesNamesWithoutRegardToCase)
{
    Put("JAMMI_A1.WAV", "sample");
    fs::create_directory(Host("Sub"));
    Put("Sub/Opts.Json", "{}");

    FIL  f;
    char buf[8] = {};
    UINT n      = 0;
    ASSERT_EQ(f_open(&f, "jammi_a1.wav", FA_READ), FR_OK);
    ASSERT_EQ(f_read(&f, buf, 6, &n), FR_OK);
    EXPECT_STREQ(buf, "sample");
    ASSERT_EQ(f_close(&f), FR_OK);
    ASSERT_EQ(f_stat("sub/opts.json", nullptr), FR_OK);

    ASSERT_EQ(f_open(&f, "SUB/New_File.txt", FA_CREATE_NEW | FA_WRITE), FR_OK);
    ASSERT_EQ(f_close(&f), FR_OK);
    EXPECT_TRUE(fs::exists(Host("Sub/New_File.txt"))) << "new files keep the firmware's name";
    EXPECT_EQ(f_open(&f, "jammi_A1.wav", FA_CREATE_NEW | FA_WRITE), FR_EXIST);

    FILINFO info;
    ASSERT_EQ(f_stat("jammi_a1.wav", &info), FR_OK);
    EXPECT_STREQ(info.fname, "JAMMI_A1.WAV") << "reports the name on the card";
    EXPECT_STREQ(info.altname, "JAMMI_A1.WAV");
}

TEST_F(Card, StaysInsideTheFolder)
{
    FIL f;
    EXPECT_EQ(f_open(&f, "../escape.txt", FA_CREATE_ALWAYS | FA_WRITE), FR_INVALID_NAME);
    EXPECT_EQ(f_open(&f, "0:/../escape.txt", FA_CREATE_ALWAYS | FA_WRITE), FR_INVALID_NAME);
    ASSERT_EQ(f_mkdir("a"), FR_OK);
    EXPECT_EQ(f_open(&f, "a/../../escape.txt", FA_CREATE_ALWAYS | FA_WRITE), FR_INVALID_NAME);
    EXPECT_EQ(f_stat("..", nullptr), FR_INVALID_NAME);
    EXPECT_FALSE(fs::exists(dir.path().parent_path() / "escape.txt"));

    EXPECT_EQ(f_open(&f, "1:/x.txt", FA_READ), FR_INVALID_DRIVE) << "only drive 0 exists";
    EXPECT_EQ(f_open(&f, "bad?.txt", FA_CREATE_ALWAYS | FA_WRITE), FR_INVALID_NAME);
    EXPECT_EQ(f_open(&f, "/abs.txt", FA_CREATE_ALWAYS | FA_WRITE), FR_OK) << "/ is the card's root";
    ASSERT_EQ(f_close(&f), FR_OK);
    EXPECT_TRUE(fs::exists(Host("abs.txt")));
}

TEST_F(Card, ReportsFreeSpace)
{
    DWORD  clusters = 0;
    FATFS* volume   = nullptr;
    ASSERT_EQ(f_getfree("0:", &clusters, &volume), FR_OK);
    EXPECT_EQ(volume, &fsi.GetSDFileSystem());
    EXPECT_GT(clusters, 0u);
    EXPECT_EQ(volume->csize, 64) << "32 KB clusters";
    EXPECT_GE(volume->n_fatent, clusters);
}

TEST_F(Card, IsNotReadyWhileOut)
{
    Put("x.txt", "x");
    FIL f;
    ASSERT_EQ(f_open(&f, "x.txt", FA_READ), FR_OK);

    SdSetPresent(false);
    char    buf[2];
    UINT    n;
    FIL     g;
    DIR     d;
    FILINFO info;
    EXPECT_EQ(f_read(&f, buf, 1, &n), FR_NOT_READY);
    EXPECT_EQ(f_lseek(&f, 0), FR_NOT_READY);
    EXPECT_EQ(f_open(&g, "x.txt", FA_READ), FR_NOT_READY);
    EXPECT_EQ(f_stat("x.txt", &info), FR_NOT_READY);
    EXPECT_EQ(f_opendir(&d, "/"), FR_NOT_READY);
    EXPECT_EQ(f_mkdir("d"), FR_NOT_READY);
    EXPECT_EQ(f_unlink("x.txt"), FR_NOT_READY);
    EXPECT_EQ(f_rename("x.txt", "y.txt"), FR_NOT_READY);
    EXPECT_EQ(f_chmod("x.txt", 0, 0), FR_NOT_READY);
    EXPECT_EQ(f_mount(&fsi.GetSDFileSystem(), "0:", 1), FR_NOT_READY);

    SdSetPresent(true);
    ASSERT_EQ(f_mount(&fsi.GetSDFileSystem(), "0:", 1), FR_OK);
    ASSERT_EQ(f_open(&f, "x.txt", FA_READ), FR_OK) << "a remount drops the old files";
    ASSERT_EQ(f_read(&f, buf, 1, &n), FR_OK);
    EXPECT_EQ(n, 1u);
    ASSERT_EQ(f_close(&f), FR_OK);
}

TEST_F(Card, IsNotReadyOnceTheFolderIsDeleted)
{
    Put("x.txt", "x");
    FIL f;
    ASSERT_EQ(f_open(&f, "x.txt", FA_READ | FA_WRITE), FR_OK);
    fs::remove_all(dir.path());

    char buf[2];
    UINT n;
    FIL  g;
    EXPECT_EQ(f_read(&f, buf, 1, &n), FR_NOT_READY);
    EXPECT_EQ(f_write(&f, "y", 1, &n), FR_NOT_READY);
    EXPECT_EQ(f_open(&g, "new.txt", FA_CREATE_ALWAYS | FA_WRITE), FR_NOT_READY);
    EXPECT_EQ(f_close(&f), FR_NOT_READY) << "but the file is closed";
    EXPECT_EQ(f_close(&f), FR_INVALID_OBJECT);
    EXPECT_FALSE(fs::exists(dir.path())) << "never recreated";
}

TEST_F(Card, NeedsAMountedVolume)
{
    f_mount(nullptr, "0:", 0);
    FIL f;
    EXPECT_EQ(f_open(&f, "x.txt", FA_CREATE_ALWAYS | FA_WRITE), FR_NOT_ENABLED);
    EXPECT_EQ(f_mount(&fsi.GetSDFileSystem(), "2:", 1), FR_INVALID_DRIVE);
}

TEST_F(Card, HasFatFsStringFunctions)
{
    FIL f;
    ASSERT_EQ(f_open(&f, "log.txt", FA_CREATE_ALWAYS | FA_WRITE | FA_READ), FR_OK);
    EXPECT_EQ(f_puts("one\ntwo\n", &f), 10) << "with CRs";
    EXPECT_EQ(f_putc('!', &f), 1);
    EXPECT_GT(f_printf(&f, "[%d|%5u|%-4s|%08X|%x|%b|%c|%ld]\n", -42, 7u, "ab", 0xBEEFu, 0xabu, 5u,
                       'q', 123456789L),
              0);
    ASSERT_EQ(f_close(&f), FR_OK);
    EXPECT_EQ(Get("log.txt"), "one\r\ntwo\r\n![-42|    7|ab  |0000BEEF|ab|101|q|123456789]\r\n");

    ASSERT_EQ(f_open(&f, "log.txt", FA_READ), FR_OK);
    char line[64];
    ASSERT_NE(f_gets(line, sizeof line, &f), nullptr);
    EXPECT_STREQ(line, "one\n") << "CRs dropped";
    ASSERT_NE(f_gets(line, 3, &f), nullptr);
    EXPECT_STREQ(line, "tw") << "stops when the buffer is full";
    ASSERT_EQ(f_close(&f), FR_OK);
}

TEST_F(Card, UsesTheFolderNameAsTheLabel)
{
    char  label[12];
    DWORD serial = 0;
    ASSERT_EQ(f_getlabel("0:", label, &serial), FR_OK);
    EXPECT_EQ(std::string(label), dir.path().filename().string().substr(0, 11));
    EXPECT_NE(serial, 0u);

    TempDir  parent;
    fs::path card = parent.path() / "old name";
    fs::create_directory(card);
    f_mount(nullptr, "0:", 0);
    SdInsert(card);
    ASSERT_EQ(f_mount(&fsi.GetSDFileSystem(), "0:", 1), FR_OK);
    ASSERT_EQ(f_setlabel("0:TAPE CARD"), FR_OK);
    EXPECT_TRUE(fs::is_directory(parent.path() / "TAPE CARD"));
    ASSERT_EQ(f_getlabel("", label, nullptr), FR_OK);
    EXPECT_STREQ(label, "TAPE CARD");
    EXPECT_EQ(f_setlabel("NAME.TOO.LONG"), FR_INVALID_NAME);
    FIL f;
    ASSERT_EQ(f_open(&f, "still.txt", FA_CREATE_NEW | FA_WRITE), FR_OK) << "the card stays in";
    ASSERT_EQ(f_close(&f), FR_OK);
    EXPECT_TRUE(fs::exists(parent.path() / "TAPE CARD/still.txt"));
}

TEST_F(Card, FormattingEmptiesTheFolder)
{
    Put("a.wav", "a");
    fs::create_directories(Host("deep/er"));
    Put("deep/er/b.txt", "b");
    BYTE work[512];
    ASSERT_EQ(f_mkfs("0:", FM_FAT32, 0, work, sizeof work), FR_OK);
    EXPECT_TRUE(fs::is_directory(dir.path()));
    EXPECT_TRUE(fs::is_empty(dir.path()));
}

TEST_F(Card, ExpandsAFile)
{
    FIL f;
    ASSERT_EQ(f_open(&f, "rec.wav", FA_CREATE_ALWAYS | FA_WRITE), FR_OK);
    EXPECT_EQ(f_expand(&f, 4096, 0), FR_OK);
    EXPECT_EQ(f_size(&f), 0u) << "opt 0 only checks for room";
    ASSERT_EQ(f_expand(&f, 4096, 1), FR_OK);
    EXPECT_EQ(f_size(&f), 4096u);
    EXPECT_EQ(f_expand(&f, 4096, 1), FR_DENIED) << "only an empty file";
    ASSERT_EQ(f_close(&f), FR_OK);
    EXPECT_EQ(fs::file_size(Host("rec.wav")), 4096u);
}

TEST_F(Card, AttributesAndTimesAreNoOps)
{
    Put("x.txt", "");
    FILINFO info = {};
    EXPECT_EQ(f_chmod("x.txt", AM_RDO, AM_RDO), FR_OK);
    EXPECT_EQ(f_utime("x.txt", &info), FR_OK);
    ASSERT_EQ(f_stat("x.txt", &info), FR_OK);
    EXPECT_EQ(info.fattrib, 0);
}

TEST_F(Card, RefusesWhatAFolderCantDo)
{
    Put("x.txt", "x");
    FIL f;
    ASSERT_EQ(f_open(&f, "x.txt", FA_READ), FR_OK);
    UINT sent = 0;
    EXPECT_EQ(f_forward(&f, [](const BYTE*, UINT n) { return n; }, 1, &sent), FR_INT_ERR);
    ASSERT_EQ(f_close(&f), FR_OK);
    const DWORD sizes[] = {100, 0, 0, 0};
    BYTE        work[512];
    EXPECT_EQ(f_fdisk(0, sizes, work), FR_INT_ERR);
}

TEST_F(Card, ReadsTheTapeFactoryCard)
{
    const fs::path factory = DAISYCOLA_TAPE_CARD_DIR;
    if(factory.empty())
        GTEST_SKIP() << "CHOMPI not available";
    f_mount(nullptr, "0:", 0);
    SdInsert(factory); // read only: nothing here writes
    ASSERT_EQ(f_mount(&fsi.GetSDFileSystem(), "0:", 1), FR_OK);

    std::vector<std::string> expected;
    for(const auto& e : fs::directory_iterator(factory))
        expected.push_back(e.path().filename().string());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(List("/"), expected);

    FILINFO info;
    ASSERT_EQ(f_stat("CUBBI_A1.WAV", &info), FR_OK);
    EXPECT_EQ(info.fsize, fs::file_size(factory / "cubbi_a1.wav"));
}

TEST(SdCard, InsertNeedsAFolder)
{
    TempDir dir;
    EXPECT_THROW(SdInsert(dir / "missing"), SdError);
    std::ofstream(dir / "file") << "x";
    EXPECT_THROW(SdInsert(dir / "file"), SdError);
    EXPECT_THROW(SdInsert(""), SdError);
    EXPECT_FALSE(SdBusy());
}
