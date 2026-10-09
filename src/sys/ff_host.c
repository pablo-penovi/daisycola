/* FatFs, compiled for the host.
 *
 * CHOMPI firmware calls f_write(fp, buf, n, NULL) when it doesn't care how many bytes were
 * written. FatFs stores the count through that pointer. On the STM32, address 0 is ITCM RAM and
 * the store goes unnoticed; on Linux it crashes. This file compiles libDaisy's ff.c unchanged,
 * with f_write renamed, and puts a wrapper in front that substitutes a scratch counter.
 */
#define f_write daisycola_ff_write
#include "ff.c"
#undef f_write

FRESULT f_write(FIL* fp, const void* buff, UINT btw, UINT* bw)
{
    UINT ignored;
    return daisycola_ff_write(fp, buff, btw, bw ? bw : &ignored);
}
