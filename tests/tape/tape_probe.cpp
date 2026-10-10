// Built into the test's TAPE library only: reads TAPE's boot flags (chompi_main.cpp) for
// power_cycle_test.cpp, which looks the function up with dlsym for each poll.
extern bool booting;
extern bool rainbow_done;

#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))
// TAPE writes these plain bools; reading them here is a race only as far as the test goes.
extern "C" void
AnnotateBenignRaceSized(const char* file, int line, const volatile void* mem, long size, const char* desc);
#define BENIGN_RACE(var) AnnotateBenignRaceSized(__FILE__, __LINE__, &(var), sizeof(var), #var)
#else
#define BENIGN_RACE(var)
#endif

extern "C" __attribute__((visibility("default"))) int tape_probe_booted()
{
    BENIGN_RACE(booting);
    BENIGN_RACE(rainbow_done);
    return !__atomic_load_n(&booting, __ATOMIC_RELAXED)
           && __atomic_load_n(&rainbow_done, __ATOMIC_RELAXED);
}
