// Minimal replacement for tchar.h provided by the FBNeo frontend (burner) (ADR-002 list B)
// Non-Unicode builds only.
#ifndef REI_SHIM_TCHAR_H
#define REI_SHIM_TCHAR_H

#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef char TCHAR;

#define _T(x)         x
#define _tcslen       strlen
#define _tcscpy       strcpy
#define _tcsncpy      strncpy
#define _tcscmp       strcmp
#define _tcsicmp      strcasecmp
#define _tcsstr       strstr
#define _stprintf     sprintf
#define _sntprintf    snprintf
#define _vsnprintf    vsnprintf
#define _tfopen       fopen
#define _ftprintf     fprintf
#define _tcstol       strtol
#define _istspace     isspace
#define _tcschr       strchr
#define _tcsrchr      strrchr
#define _tcsncmp      strncmp
#define _tcscat       strcat
#define _stricmp      strcasecmp
#define _strnicmp     strncasecmp

#endif
