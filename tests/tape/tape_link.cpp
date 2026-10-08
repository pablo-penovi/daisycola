// Links the TAPE firmware against daisycola. Building this test is the check: every libDaisy
// symbol TAPE uses must be defined. Running it only confirms the firmware entry point exists.
#include <cstdio>

int tape_main();

int main()
{
    std::printf("TAPE linked, entry point at %p\n", reinterpret_cast<void*>(&tape_main));
    return 0;
}
