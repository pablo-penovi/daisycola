/* daisycola: fixed-width replacement for FatFs's integer.h.
 *
 * FatFs requires DWORD to be 32 bits, but its integer.h uses `unsigned long`, which is 64 bits on
 * x86-64. This header defines the types with exact widths and sets integer.h's include guard.
 * daisycola's CMake target force-includes it into every file, so no file can see the 64-bit
 * types and every file agrees on the layout of FatFs's structs.
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
