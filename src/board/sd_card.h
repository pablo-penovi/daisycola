// The virtual SD card: a folder on the host.
//
// The host inserts and ejects it through host.h. The firmware reaches it through the FatFs API,
// which sys/ff_folder.cpp implements on top of the folder. Both run on the firmware's thread,
// sometimes inside an interrupt (TAPE streams samples from its timer callback), so nothing here
// allocates or locks.
#pragma once

namespace daisycola::sd
{
/** The inserted card's folder, open as a directory, or -1 with no card. */
int FolderFd();

/** True while a card is inserted and present, and its folder still exists. */
bool Ready();

/** The card folder's host path; empty with no card. */
const char* FolderPath();

/** Renames the card folder within its parent (f_setlabel). Returns 0 or an errno value. */
int RenameFolder(const char* name);

/** Counts a card access, so the host can tell the firmware is in the middle of one (SdBusy).
 *  Accesses nest: a timer interrupt can reach the card while the main loop is using it. */
class BusyScope
{
  public:
    BusyScope();
    ~BusyScope();
    BusyScope(const BusyScope&)            = delete;
    BusyScope& operator=(const BusyScope&) = delete;
};

/** Closes every file and directory the firmware has open on the card (sys/ff_folder.cpp). */
void CloseOpenFiles();

} // namespace daisycola::sd
