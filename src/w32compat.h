// Minimal POSIX compatibility shim for compiling linuxdoom-1.10 as C++ on
// Windows (UCRT / zig cc). File I/O, dir access and strcasecmp family.
#ifndef W32COMPAT_H
#define W32COMPAT_H

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
// Windows' RPC headers typedef a lowercase `boolean` that clashes with
// DOOM's; hide it for the duration of the include, then let DOOM's own
// doomtype.h define boolean as it wishes.
#define boolean rpc_boolean_conflict
#include <windows.h>
#undef boolean
// DOOM has its own LoadMenu(); windows.h maps it to LoadMenuA.
#undef LoadMenu
#include <io.h>       // _open, _close, _read, _write, _lseek, _access
#include <direct.h>   // _mkdir
#include <string.h>
#include <fcntl.h>
#include <stdio.h>

// --- open-family flags (UCRT _O_* values) ---
#ifndef O_RDONLY
#  define O_RDONLY 0x0000
#endif
#ifndef O_WRONLY
#  define O_WRONLY 0x0001
#endif
#ifndef O_RDWR
#  define O_RDWR   0x0002
#endif
#ifndef O_APPEND
#  define O_APPEND 0x0008
#endif
#ifndef O_CREAT
#  define O_CREAT  0x0100
#endif
#ifndef O_TRUNC
#  define O_TRUNC  0x0200
#endif
#ifndef O_EXCL
#  define O_EXCL   0x0400
#endif
#ifndef O_BINARY
#  define O_BINARY 0x8000
#endif

// --- access() modes ---
#ifndef R_OK
#  define R_OK 4
#  define W_OK 2
#  define F_OK 0
#endif
#define X_OK F_OK   // no exec bit on Windows

// --- POSIX -> CRT name mapping ---
#define open      _open
#define close     _close
#define read      _read
#define write     _write
#define lseek     _lseek
#define access    _access
#define unlink    _unlink
#define strdup    _strdup
#define strcasecmp    _stricmp
#define strncasecmp   _strnicmp

// mkdir took a mode under POSIX; ignore it.
static inline int mkdir(const char* path, int /*mode*/)
{
    return _mkdir(path);
}

#endif // W32COMPAT_H
