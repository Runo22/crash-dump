////////////////////////////////////////////////////////////////////////////////
// User Defined Headers
#include "crash_report.hpp"
#include "crash_io.hpp"
#include "crash_modules.hpp"
#include "crash_symbols.hpp"
////////////////////////////////////////////////////////////////////////////////
// Windows Headers
#include <psapi.h>
#include <dbghelp.h>
////////////////////////////////////////////////////////////////////////////////

namespace crash::report {
namespace {

constexpr size_t kMaxFrames = 128;
constexpr size_t kPathMax   = 1024;

const char* exception_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case 0xE06D7363:                      return "C++ EXCEPTION (unhandled throw)";
        case 0xC0000374:                      return "HEAP_CORRUPTION";
        default:                              return "UNKNOWN";
    }
}

void write_header(HANDLE file, const Context& ctx) {
    SYSTEMTIME st;
    GetLocalTime(&st);

    char name[256], ver[128];
    io::narrow(name, sizeof(name), ctx.app_name);
    io::narrow(ver,  sizeof(ver),  ctx.app_version);

    io::write_str(file, "=== CRASH REPORT ===\r\n");
    io::write_fmt(file, "time      : %04u-%02u-%02u %02u:%02u:%02u\r\n",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    io::write_fmt(file, "app       : %s %s\r\n", name, ver);
    io::write_fmt(file, "pid / tid : %lu / %lu\r\n",
                  GetCurrentProcessId(), ctx.thread_id);

    if (ctx.log_file && ctx.log_file[0]) {
        char buf[kPathMax];
        io::narrow(buf, sizeof(buf), ctx.log_file);
        io::write_fmt(file, "app log   : %s\r\n", buf);
    }

    if (ctx.reason && ctx.reason[0]) {
        char buf[512];
        io::narrow(buf, sizeof(buf), ctx.reason);
        io::write_fmt(file, "reason    : %s\r\n", buf);
    }

    if (ctx.exception && ctx.exception->ExceptionRecord) {
        const EXCEPTION_RECORD* er = ctx.exception->ExceptionRecord;
        io::write_fmt(file, "exception : 0x%08X  %s\r\n",
                      er->ExceptionCode, exception_name(er->ExceptionCode));

        char  mod[128];
        DWORD rva = 0;
        modules::describe(er->ExceptionAddress, mod, sizeof(mod), rva);
        io::write_str(file, "fault at  : ");
        io::write_ptr(file, er->ExceptionAddress);
        io::write_fmt(file, "  %s+0x%08X\r\n", mod, rva);

        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
            const ULONG_PTR op = er->ExceptionInformation[0];
            io::write_fmt(file, "access    : %s at ",
                          op == 0 ? "read" : (op == 1 ? "write" : "execute"));
            io::write_ptr(file, reinterpret_cast<void*>(er->ExceptionInformation[1]));
            io::write_str(file, "\r\n");
        }
    }

    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        io::write_fmt(file, "workingset: %lu MB (peak %lu MB)\r\n",
                      static_cast<DWORD>(pmc.WorkingSetSize / (1024 * 1024)),
                      static_cast<DWORD>(pmc.PeakWorkingSetSize / (1024 * 1024)));
    }
}

} // namespace

void write_text(const wchar_t* path, const Context& ctx) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    write_header(file, ctx);

    void*  frames[kMaxFrames]{};
    size_t count = 0;
    if (ctx.thread && ctx.exception && ctx.exception->ContextRecord)
        count = symbols::write_stack(file, ctx.thread, ctx.exception->ContextRecord,
                                     frames, kMaxFrames);

    modules::write_table(file, frames, count);

    io::write_str(file, "\r\n=== end ===\r\n");
    FlushFileBuffers(file);
    CloseHandle(file);
}

void write_minidump(const wchar_t* path, const Context& ctx) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    // Every thread's stack, the heap those stacks reference, and the VA layout
    // needed to diagnose corruption -- without the multi-GB cost of
    // MiniDumpWithFullMemory. IgnoreInaccessibleMemory matters: one bad page
    // can otherwise fail the whole dump and leave nothing behind.
    const auto type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
        MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory |
        MiniDumpScanMemory | MiniDumpWithProcessThreadData |
        MiniDumpWithFullMemoryInfo | MiniDumpIgnoreInaccessibleMemory);

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId          = ctx.thread_id;
    mei.ExceptionPointers = ctx.exception;
    mei.ClientPointers    = FALSE;

    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                      file, type, ctx.exception ? &mei : nullptr,
                                      nullptr, nullptr);

    FlushFileBuffers(file);
    CloseHandle(file);

    // A failed dump leaves a truncated file that looks valid; drop it so only
    // the reliable text log remains rather than a misleading .dmp.
    if (!ok) DeleteFileW(path);
}

} // namespace crash::report
