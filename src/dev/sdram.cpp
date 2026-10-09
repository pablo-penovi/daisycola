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
} // namespace

void daisycola::board::MapSdram()
{
#ifdef DAISYCOLA_ASAN
    // On x86-64, AddressSanitizer's shadow gap covers 0xC0000000. With ASAN_OPTIONS
    // protect_shadow_gap=0 the gap is left unmapped and ASan treats it as ordinary memory, so the
    // range can be mapped along with its own (zeroed, so addressable) shadow.
    constexpr uintptr_t kShadowOffset = 0x7fff8000u;
    if(!MapAt((kSdramBase >> 3) + kShadowOffset, kSdramSize >> 3))
    {
        std::fprintf(stderr,
                     "daisycola: SDRAM at 0xC0000000 needs ASAN_OPTIONS=protect_shadow_gap=0 "
                     "under AddressSanitizer; firmware that uses raw SDRAM addresses will crash\n");
        return;
    }
#endif
    if(!MapAt(kSdramBase, kSdramSize))
        std::fprintf(stderr,
                     "daisycola: can't map SDRAM at 0xC0000000; firmware that uses raw SDRAM "
                     "addresses will crash\n");
}
