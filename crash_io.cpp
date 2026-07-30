////////////////////////////////////////////////////////////////////////////////
// Standard Headers
#include <cstdarg>
#include <cstring>
////////////////////////////////////////////////////////////////////////////////
// User Defined Headers
#include "crash_io.hpp"
////////////////////////////////////////////////////////////////////////////////

namespace crash::io {

void write_all(HANDLE file, const char* data, size_t len) {
    DWORD written = 0;
    while (len > 0) {
        if (!WriteFile(file, data, static_cast<DWORD>(len), &written, nullptr) || written == 0)
            return;
        data += written;
        len  -= written;
    }
}

void write_str(HANDLE file, const char* s) { write_all(file, s, std::strlen(s)); }

void write_fmt(HANDLE file, const char* fmt, ...) {
    char    buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    if (n > 0) write_all(file, buf, static_cast<size_t>(n));
}

void write_ptr(HANDLE file, const void* p) {
    const auto v = reinterpret_cast<uintptr_t>(p);
    if constexpr (sizeof(uintptr_t) == 8) {
        // Emitted as two 32-bit halves because wvsprintfA has no %p.
        write_fmt(file, "0x%08X%08X",
                  static_cast<DWORD>(static_cast<unsigned long long>(v) >> 32),
                  static_cast<DWORD>(v & 0xFFFFFFFFu));
    } else {
        write_fmt(file, "0x%08X", static_cast<DWORD>(v));
    }
}

void narrow(char* dst, int cap, const wchar_t* src) {
    if (!src || !*src) { dst[0] = '\0'; return; }
    if (WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, cap, nullptr, nullptr) == 0)
        dst[0] = '\0';
}

void copy_ascii(char* dst, size_t cap, const char* src) {
    if (cap == 0) return;              // no room even for the terminator
    if (!src) { dst[0] = '\0'; return; }
    size_t n = std::strlen(src);
    if (n > cap - 1) n = cap - 1;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

} // namespace crash::io
