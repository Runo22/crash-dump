#pragma once

/// @brief Registry of loaded binaries and their build identity. Internal.
///
/// Populated at load time by crash::register_module(), read back inside the
/// handler where nothing may allocate.

////////////////////////////////////////////////////////////////////////////////
// User Defined Headers
#include "crash_handler.hpp"
////////////////////////////////////////////////////////////////////////////////
// Windows Headers
#include <windows.h>
////////////////////////////////////////////////////////////////////////////////

namespace crash::modules {

/// @brief Record a module. Fixed slot count; extra modules are ignored.
void add(const wchar_t* display_name, void* module_base, const GitInfo& git);

/// @brief Flag a module as unloaded, keeping its record.
void mark_unloaded(void* module_base);

/// @brief Name any address, registered or not.
///
/// Falls back to the loader so third-party DLLs (Qt, drivers) still get named.
/// @param out     Receives the module name; never left empty.
/// @param out_rva Receives the offset from the module base.
void describe(const void* addr, char* out, int cap, DWORD& out_rva);

/// @brief Write the module sections of the report: first the ones appearing
///        in @p frames, then the full registry.
void write_table(HANDLE file, void* const* frames, size_t frame_count);

} // namespace crash::modules
