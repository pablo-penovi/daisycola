// A firmware that never halts: it spins without a delay or a clock read, where Halt parks.
int main()
{
    for(;;)
        __asm__ volatile("");
}
