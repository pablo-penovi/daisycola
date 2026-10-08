// Phase 1 stubs: link-only bodies that abort if reached.
#pragma once
#include <cstdio>
#include <cstdlib>

#define DAISYCOLA_STUB()                                                 \
    do                                                                   \
    {                                                                    \
        std::fprintf(stderr, "daisycola: %s is not implemented yet\n", \
                     __PRETTY_FUNCTION__);                               \
        std::abort();                                                    \
    } while(0)
