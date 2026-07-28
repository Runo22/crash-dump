#pragma once

/// @brief Public API. Windows crash reporting: minidump + symbolized text log.
///
/// Reports are written from a dedicated thread with its own stack, so a stack
/// overflow is still reportable. No `<windows.h>` here.

////////////////////////////////////////////////////////////////////////////////
// Standard Headers
#include <string>
#include <string_view>
////////////////////////////////////////////////////////////////////////////////

namespace crash {

/// @brief Build identity baked into a binary. POD and fixed-size on purpose:
///        filled at load time, read inside the handler where nothing may
///        allocate.
struct GitInfo {
    const char* hash    = "";     ///< Short or full sha.
    const char* message = "";     ///< First line only; the rest is noise here.
    bool        dirty   = false;  ///< Uncommitted changes at build time.
};

/// @brief Startup configuration.
struct Config {
    /// Report directory, created if missing. Prefer local disk: a network
    /// share may be unreachable at crash time. Use @ref mirror_dir for LAN.
    std::wstring dump_dir = L"crashes";

    /// Optional second copy, e.g. `\\\\server\\share\\crashes`. Written only
    /// after the local copy succeeds. Empty disables it.
    std::wstring mirror_dir;

    std::wstring app_name    = L"app";
    std::wstring app_version = L"0.0.0";  ///< Must match the archived PDBs.

    /// Identity of the executable itself. Plugins register their own.
    GitInfo git;

    /// Optional path to the application log, recorded in the report header so
    /// the two artifacts can be paired later.
    /// @note The handler never touches that file. Ensure your logger flushes
    ///       eagerly, or the lines leading up to the crash will still be
    ///       sitting in a buffer.
    std::wstring log_file;
};

/// @brief Install the crash hooks. Call once, first thing in main().
/// @note Not thread safe; nothing else may be running yet.
bool install(const Config& cfg);

/// @brief Give a secondary thread the reserved stack needed to report a stack
///        overflow. Call once from each long-lived worker you spawn.
/// @note install() does this for the main thread only -- the guarantee is
///       per-thread.
void protect_current_thread();

/// @brief Record a module so the report can name it and its commit.
/// @param display_name Short label used in the report, e.g. `radar_plugin`.
/// @param module_base  HMODULE from LoadLibrary.
/// @note Call from the module handler right after LoadLibrary succeeds.
///       Deliberately not queried at crash time: calling GetProcAddress into
///       an unloading DLL while handling a fault turns one crash into two.
void register_module(const wchar_t* display_name, void* module_base, const GitInfo& git);

/// @brief Flag a module as unloaded. Call immediately before FreeLibrary.
/// @note The record is kept, not erased. A crash caused by a dangling
///       callback into an unloaded plugin needs that plugin's identity more
///       than any other report does.
void mark_module_unloaded(void* module_base);

/// @brief Produce a report without crashing, then continue -- for watchdogs
///        and "something is wrong, capture state" paths.
/// @return Report id (the timestamp stem shared by the .dmp and .log).
std::wstring write_report_now(std::wstring_view reason);

} // namespace crash
