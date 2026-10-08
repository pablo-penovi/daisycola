/* daisycola: fixed-width replacement for FatFs's integer.h.
 *
 * FatFs requires DWORD to be 32 bits, but its integer.h uses `unsigned long`, which is 64 bits on
 * x86-64. This header defines the types with exact widths and sets integer.h's include guard, so
 * it has to be included before any FatFs header. daisycola's ff.h, diskio.h and fatfs.h wrappers
 * do that, and the FatFs sources are compiled with it force-included.
 */
#ifndef _FF_INTEGER
#define _FF_INTEGER

#include <stdint.h>

typedef int                INT;
typedef unsigned int       UINT;
typedef unsigned char      BYTE;
typedef short              SHORT;
typedef unsigned short     WORD;
typedef unsigned short     WCHAR;
typedef int32_t            LONG;
typedef uint32_t           DWORD;
typedef unsigned long long QWORD;

#endif
