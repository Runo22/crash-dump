#pragma once

/// @brief Report file generation. Internal.

////////////////////////////////////////////////////////////////////////////////
// Windows Headers
#include <windows.h>
////////////////////////////////////////////////////////////////////////////////

namespace crash::report {

/// @brief Everything the writer thread needs. Plain pointers: the owning
///        state is static and outlives the report.
struct Context {
    const wchar_t*      app_name    = nullptr;
    const wchar_t*      app_version = nullptr;
    const wchar_t*      log_file    = nullptr;  ///< May be empty.
    const wchar_t*      reason      = nullptr;  ///< May be empty.
    EXCEPTION_POINTERS* exception   = nullptr;  ///< Null for a voluntary report.
    DWORD               thread_id   = 0;
    HANDLE              thread      = nullptr;  ///< Real handle, not the pseudo-handle.
};

/// @brief Symbolized text summary: header, call stack, module identity.
void write_text(const wchar_t* path, const Context& ctx);

/// @brief Minidump tuned for a thread-heavy process with third-party
///        libraries. Typically 30-150 MB.
void write_minidump(const wchar_t* path, const Context& ctx);

} // namespace crash::report
