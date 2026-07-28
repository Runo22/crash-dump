////////////////////////////////////////////////////////////////////////////////
// User Defined Headers
#include "crash_symbols.hpp"
#include "crash_io.hpp"
#include "crash_modules.hpp"
////////////////////////////////////////////////////////////////////////////////
// Windows Headers
#include <dbghelp.h>
////////////////////////////////////////////////////////////////////////////////

#pragma comment(lib, "dbghelp.lib")

namespace crash::symbols {
namespace {

bool g_ready = false;

/// @brief Lazily initialize dbghelp.
/// @note A null search path means "exe directory + `_NT_SYMBOL_PATH`".
///       Offline, point `_NT_SYMBOL_PATH` at the LAN symbol folder or leave
///       the PDBs beside the binaries.
void ensure_init() {
    if (g_ready) return;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES |
                  SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    g_ready = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
}

} // namespace

size_t write_stack(HANDLE file, HANDLE thread, const CONTEXT* ctx_in,
                   void** frames_out, size_t frames_cap) {
    ensure_init();

    CONTEXT      ctx = *ctx_in;  // StackWalk64 mutates it
    STACKFRAME64 frame{};
    DWORD        machine;

#if defined(_M_X64)
    machine                = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset    = ctx.Rip;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_ARM64)
    machine                = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset    = ctx.Pc;
    frame.AddrFrame.Offset = ctx.Fp;
    frame.AddrStack.Offset = ctx.Sp;
#else
    machine                = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset    = ctx.Eip;
    frame.AddrFrame.Offset = ctx.Ebp;
    frame.AddrStack.Offset = ctx.Esp;
#endif
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;

    // SYMBOL_INFO needs trailing room for the undecorated name.
    alignas(SYMBOL_INFO) char sym_buf[sizeof(SYMBOL_INFO) + 1024]{};
    auto* sym         = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = 1024;

    const HANDLE proc  = GetCurrentProcess();
    size_t       count = 0;

    io::write_str(file, "\r\n--- call stack (faulting thread) ---\r\n");

    while (count < frames_cap) {
        if (!StackWalk64(machine, proc, thread, &frame, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (frame.AddrPC.Offset == 0) break;

        const auto pc = reinterpret_cast<void*>(frame.AddrPC.Offset);

        // Module + RVA never fails and is enough to resolve the frame offline
        // even with no PDB on this machine. The symbol name is a bonus.
        char  mod[128];
        DWORD rva = 0;
        modules::describe(pc, mod, sizeof(mod), rva);

        io::write_fmt(file, "#%02u  ", static_cast<unsigned>(count));
        io::write_ptr(file, pc);
        io::write_fmt(file, "  %s+0x%08X", mod, rva);

        DWORD64 disp = 0;
        if (SymFromAddr(proc, frame.AddrPC.Offset, &disp, sym)) {
            io::write_fmt(file, "  %s+0x%X", sym->Name, static_cast<DWORD>(disp));

            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD line_disp   = 0;
            if (SymGetLineFromAddr64(proc, frame.AddrPC.Offset, &line_disp, &line))
                io::write_fmt(file, "  [%s:%lu]", line.FileName, line.LineNumber);
        }
        io::write_str(file, "\r\n");

        frames_out[count++] = pc;
    }

    if (count == 0)
        io::write_str(file, "(no frames -- check /Oy- and that PDBs are "
                            "reachable; use the minidump instead)\r\n");
    return count;
}

} // namespace crash::symbols
