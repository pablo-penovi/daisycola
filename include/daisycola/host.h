/* daisycola host API: how a host program drives the virtual Daisy board.
 *
 * The firmware runs on its own thread. Everything here is meant to be called from host threads
 * unless it says otherwise.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace daisycola
{
// ---- Firmware ----------------------------------------------------------------------------------

/** The firmware's main function, renamed at compile time with -Dmain=<name>. */
using FirmwareMain = int (*)();

/** Starts the firmware on its own thread. */
void Start(FirmwareMain firmware_main);

// ---- SD card -----------------------------------------------------------------------------------
//
// The card is a disk-image file: an MBR with one FAT32 partition, like a real microSD card. It
// can be loop-mounted or used with mtools. The helpers below format, fill and read the image
// through FatFs on the calling thread. They must not run while the firmware is running.

/** Thrown by the SD-card helpers. */
class SdError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

/** Creates a sparse image file of `size_bytes` and formats it as FAT32. Overwrites `path`. */
void SdCreateImage(const std::string& path, uint64_t size_bytes);

/** Inserts the card: the firmware reads and writes this image file from now on. */
void SdOpenImage(const std::string& path);

/** Removes the card and closes the image file. */
void SdCloseImage();

/** Simulates pulling the card out (false) or putting it back (true) without closing the image. */
void SdSetPresent(bool present);

/** True while the firmware is in the middle of a card read or write. */
bool SdBusy();

/** One file or directory on the card. */
struct SdEntry
{
    std::string name;
    uint64_t    size;
    bool        is_dir;
};

/** Lists a directory on the card ("/" for the root). */
std::vector<SdEntry> SdList(const std::string& card_dir = "/");

/** Copies a host file, or a host directory recursively, into a directory on the card. */
void SdCopyIn(const std::string& host_path, const std::string& card_dir = "/");

/** Copies a card file, or a card directory recursively, into a host directory. */
void SdCopyOut(const std::string& card_path, const std::string& host_dir);

/** Reads a whole file from the card. */
std::vector<uint8_t> SdReadFile(const std::string& card_path);

} // namespace daisycola
