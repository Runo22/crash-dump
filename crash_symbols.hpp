#pragma once

/// @brief dbghelp stack walking and symbolization. Internal.

////////////////////////////////////////////////////////////////////////////////
// Windows Headers
#include <windows.h>
////////////////////////////////////////////////////////////////////////////////

namespace crash::symbols {

/// @brief Walk @p thread using @p ctx and write a symbolized trace.
/// @param thread     Must be blocked or suspended; the faulting thread is
///                   parked in WaitForSingleObject while this runs, so its
///                   stack is frozen and safe to walk.
/// @param frames_out Receives the visited program counters.
/// @return Number of frames written.
size_t write_stack(HANDLE file, HANDLE thread, const CONTEXT* ctx,
                   void** frames_out, size_t frames_cap);

} // namespace crash::symbols
