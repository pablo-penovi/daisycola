// The Seed's SDRAM. SdramHandle is declared for completeness; CHOMPI firmware never calls it.
//
// Buffers marked DSY_SDRAM_BSS are ordinary globals on the host. Some firmware also uses raw
// addresses (TAPE zeroes 0xC0000000..+64 MB at boot), so the range is mapped at its real address.
#include <cstdint>
#include <cstdio>
#include <sys/mman.h>

#include "board/board.h"

#if defined(__SANITIZE_ADDRESS__)
#define DAISYCOLA_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DAISYCOLA_ASAN 1
#endif
#endif

namespace
{
constexpr uintptr_t kSdramBase = 0xC0000000u;
constexpr size_t    kSdramSize = 64u << 20;

#ifdef DAISYCOLA_ASAN
// ASan's shadow offset on x86-64: the shadow byte of address a is at (a >> 3) + kShadowOffset.
constexpr uintptr_t kShadowOffset = 0x7fff8000u;
constexpr uintptr_t kShadowBase   = (kSdramBase >> 3) + kShadowOffset;
bool                shadow_mapped = false;
bool                sdram_lent    = false; // the range is ASan's own mapping (see MapSdram)
#endif
bool sdram_mapped = false;

bool MapAt(uintptr_t address, size_t size)
{
    void* want = reinterpret_cast<void*>(address);
    void* got  = mmap(want,
                     size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                     -1,
                     0);
    if(got == want)
        return true;
    if(got != MAP_FAILED)
        munmap(got, size); // an old kernel that took the address as a hint
    return false;
}

#ifdef DAISYCOLA_ASAN
// True if one readable and writable mapping already covers [address, address + size).
bool Covered(uintptr_t address, size_t size)
{
    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if(!maps)
        return false;
    bool          covered = false;
    unsigned long from, to;
    char          perms[8];
    char          line[512];
    while(!covered && std::fgets(line, sizeof line, maps))
        if(std::sscanf(line, "%lx-%lx %7s", &from, &to, perms) == 3)
            covered = from <= address && address + size <= to && perms[0] == 'r'
                      && perms[1] == 'w';
    std::fclose(maps);
    return covered;
}
#endif
} // namespace

void daisycola::board::MapSdram()
{
    if(sdram_mapped)
        return;
#ifdef DAISYCOLA_ASAN
    // On x86-64, AddressSanitizer's shadow gap covers 0xC0000000. With ASAN_OPTIONS
    // protect_shadow_gap=0, older ASan versions leave the gap unmapped and treat it as ordinary
    // memory, so the range can be mapped along with its own (zeroed, so addressable) shadow.
    // Newer ones map the whole gap read-write themselves: then the range is already usable, and
    // its shadow too.
    if(Covered(kSdramBase, kSdramSize) && Covered(kShadowBase, kSdramSize >> 3))
    {
        sdram_lent = true;
        return;
    }
    if(!shadow_mapped && !MapAt(kShadowBase, kSdramSize >> 3))
    {
        std::fprintf(stderr,
                     "daisycola: SDRAM at 0xC0000000 needs ASAN_OPTIONS=protect_shadow_gap=0 "
                     "under AddressSanitizer; firmware that uses raw SDRAM addresses will crash\n");
        return;
    }
    shadow_mapped = true;
#endif
    sdram_mapped = MapAt(kSdramBase, kSdramSize);
    if(!sdram_mapped)
        std::fprintf(stderr,
                     "daisycola: can't map SDRAM at 0xC0000000; firmware that uses raw SDRAM "
                     "addresses will crash\n");
}

void daisycola::board::UnmapSdram()
{
    if(sdram_mapped)
        munmap(reinterpret_cast<void*>(kSdramBase), kSdramSize);
    sdram_mapped = false;
#ifdef DAISYCOLA_ASAN
    // ASan's own mapping stays, emptied: the next power-up finds it zeroed, as after a munmap.
    if(sdram_lent)
        madvise(reinterpret_cast<void*>(kSdramBase), kSdramSize, MADV_DONTNEED);
    sdram_lent = false;
    if(shadow_mapped)
        munmap(reinterpret_cast<void*>(kShadowBase), kSdramSize >> 3);
    shadow_mapped = false;
#endif
}
