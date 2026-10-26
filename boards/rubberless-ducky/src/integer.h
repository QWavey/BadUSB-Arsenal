#ifndef _INTEGER
#define _INTEGER

#ifdef _WIN32
#include <windows.h>
#else

typedef int				INT;
typedef unsigned int	UINT;

typedef char			CHAR;
typedef unsigned char	UCHAR;
typedef unsigned char	BYTE;

typedef short			SHORT;
typedef unsigned short	USHORT;
typedef unsigned short	WORD;

typedef long			LONG;
typedef unsigned long	ULONG;
typedef unsigned long	DWORD;

typedef enum { FALSE = 0, TRUE = 1 } BOOL;

#endif
#endif