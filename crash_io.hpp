#pragma once

/// @brief CRT-free output primitives. Internal.
///
/// After heap corruption or a stack overflow the CRT is not trustworthy.
/// Nothing here allocates, takes a lock, or uses buffered I/O.

////////////////////////////////////////////////////////////////////////////////
// Windows Headers
#include <windows.h>
////////////////////////////////////////////////////////////////////////////////

namespace crash::io {

/// @brief Raw WriteFile loop.
void write_all(HANDLE file, const char* data, size_t len);

/// @brief write_all() for a NUL-terminated string.
void write_str(HANDLE file, const char* s);

/// @brief Formatted write via wvsprintfA -- a kernel formatter: no heap, no
///        locale, output capped at 1024 bytes.
/// @warning Supports only `%c %d %i %s %u %x %X` plus width, `-` and `l`.
///          There is no `%p` and no `%f`; use write_ptr() for pointers.
void write_fmt(HANDLE file, const char* fmt, ...);

/// @brief Write a pointer as hex, working around the missing `%p`.
void write_ptr(HANDLE file, const void* p);

/// @brief UTF-16 to UTF-8 into a caller-owned buffer.
void narrow(char* dst, int cap, const wchar_t* src);

/// @brief Bounded ASCII copy (no CRT string functions on the report path).
void copy_ascii(char* dst, size_t cap, const char* src);

} // namespace crash::io
