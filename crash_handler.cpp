/// @brief Hook installation, dedicated writer thread, public API.

////////////////////////////////////////////////////////////////////////////////
// Standard Headers
#include <csignal>
#include <cstdlib>    // _set_purecall_handler, _set_invalid_parameter_handler
#include <exception>
////////////////////////////////////////////////////////////////////////////////
// User Defined Headers
#include "crash_handler.hpp"
#include "crash_report.hpp"
#include "crash_modules.hpp"
////////////////////////////////////////////////////////////////////////////////
// Windows Headers
#include <new.h>      // _set_new_handler (not <new>)
#include <windows.h>
////////////////////////////////////////////////////////////////////////////////

namespace crash {
namespace {

constexpr size_t kPathMax         = 1024;
constexpr DWORD  kWriterTimeoutMs = 120 * 1000;  ///< A large dump legitimately takes seconds.

struct State {
    bool installed = false;

    wchar_t dump_dir[kPathMax]{};
    wchar_t mirror_dir[kPathMax]{};
    wchar_t app_name[128]{};
    wchar_t app_version[64]{};
    wchar_t log_file[kPathMax]{};

    HANDLE writer_thread = nullptr;
    HANDLE work_ready    = nullptr;
    HANDLE work_done     = nullptr;

    report::Context pending{};  ///< Handed to the writer thread.
    wchar_t         reason[256] = L"";
    wchar_t         stem[512]   = L"";  ///< Report id, built once and shared by .dmp/.log.

    bool break_into_debugger = true;  ///< Break into an attached debugger instead of dumping.

    LONG handling = 0;  ///< Reentrancy guard: the first crash wins.
};

State g;

void build_stem(wchar_t* out) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wsprintfW(out, L"%s_%s_%04u%02u%02u_%02u%02u%02u_%lu",
              g.app_name, g.app_version,
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
              GetCurrentProcessId());
}

/// @brief Best-effort copy to the LAN share; never blocks the local write.
void mirror(const wchar_t* src, const wchar_t* stem, const wchar_t* ext) {
    wchar_t dst[kPathMax];
    wsprintfW(dst, L"%s\\%s%s", g.mirror_dir, stem, ext);
    CopyFileW(src, dst, FALSE);
}

void produce_report() {
    CreateDirectoryW(g.dump_dir, nullptr);

    wchar_t dmp[kPathMax], log[kPathMax];
    wsprintfW(dmp, L"%s\\%s.dmp", g.dump_dir, g.stem);
    wsprintfW(log, L"%s\\%s.log", g.dump_dir, g.stem);

    report::write_text(log, g.pending);  // cheap and near-certain; do it first
    report::write_minidump(dmp, g.pending);

    // Best-effort LAN mirror. Create the share directory once, not once per file.
    if (g.mirror_dir[0]) {
        CreateDirectoryW(g.mirror_dir, nullptr);
        mirror(log, g.stem, L".log");
        mirror(dmp, g.stem, L".dmp");
    }
}

/// @brief Report writer. Its own fresh stack is what keeps stack-overflow
///        crashes reportable: the faulting thread has none left.
DWORD WINAPI writer_main(LPVOID) {
    for (;;) {
        WaitForSingleObject(g.work_ready, INFINITE);
        produce_report();
        SetEvent(g.work_done);
    }
}

/// @brief Fill @ref State::pending for the calling thread.
void capture_context(EXCEPTION_POINTERS* ep, const wchar_t* reason) {
    build_stem(g.stem);  // one id, shared by the .dmp, the .log and the returned report id
    if (reason) lstrcpynW(g.reason, reason, 256);

    g.pending.app_name    = g.app_name;
    g.pending.app_version = g.app_version;
    g.pending.log_file    = g.log_file;
    g.pending.reason      = g.reason;
    g.pending.exception   = ep;
    g.pending.thread_id   = GetCurrentThreadId();

    // GetCurrentThread() is a pseudo-handle, meaningless on the writer thread.
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &g.pending.thread, THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                    FALSE, 0);
}

/// @brief Hand the work to the writer thread and wait for it.
void run_writer() {
    if (g.writer_thread) {
        SetEvent(g.work_ready);
        WaitForSingleObject(g.work_done, kWriterTimeoutMs);  // bounded
    } else {
        produce_report();  // fallback if install() partially failed
    }
}

/// @brief The single funnel every crash path goes through.
void dispatch(EXCEPTION_POINTERS* ep, const wchar_t* reason) {
    // A second thread faulting mid-report must not re-enter.
    if (InterlockedCompareExchange(&g.handling, 1, 0) != 0) {
        Sleep(INFINITE);  // park it; the first thread owns the report
        return;
    }
    capture_context(ep, reason);
    run_writer();
}

/// @brief True when an attached debugger should handle the fault instead of us.
bool debugger_owns_fault() {
    return g.break_into_debugger && IsDebuggerPresent();
}

LONG WINAPI seh_filter(EXCEPTION_POINTERS* ep) {
    // Hand the fault back to the debugger: it breaks in with live state, and we
    // write no dump. (With a debugger attached this filter is usually skipped by
    // the OS anyway; the check keeps the two paths consistent.)
    if (debugger_owns_fault()) return EXCEPTION_CONTINUE_SEARCH;

    dispatch(ep, nullptr);
    // Do not return to the CRT: it would run atexit handlers and destructors
    // over corrupt state, turning one clean report into a second, confusing
    // crash.
    TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

/// @brief Report from a CRT path that has no EXCEPTION_POINTERS.
void report_synthetic(const wchar_t* reason, DWORD code) {
    // CRT paths (terminate, abort, pure call, ...) reach us even under a
    // debugger, unlike SEH. Break in so the fault can be inspected live rather
    // than dumped and terminated.
    if (debugger_owns_fault()) { __debugbreak(); return; }

    CONTEXT ctx{};
    RtlCaptureContext(&ctx);

    EXCEPTION_RECORD rec{};
    rec.ExceptionCode  = code;
    rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
#if defined(_M_X64)
    rec.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
#elif defined(_M_ARM64)
    rec.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Pc);
#else
    rec.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Eip);
#endif

    EXCEPTION_POINTERS ep{&rec, &ctx};
    dispatch(&ep, reason);
    TerminateProcess(GetCurrentProcess(), code);
}

/// Unhandled C++ throw, noexcept violation, or a throw during unwind.
void on_terminate() { report_synthetic(L"std::terminate", 0xE06D7363); }

void on_purecall() { report_synthetic(L"pure virtual call", 0xC0000025); }

/// CRT contract violation, e.g. an sprintf_s buffer overflow.
void on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*,
                          unsigned int, uintptr_t) {
    report_synthetic(L"CRT invalid parameter", 0xC000000D);
}

void on_signal(int sig) {
    report_synthetic(sig == SIGABRT ? L"SIGABRT" : L"signal", 0xC0000409);
}

/// @brief `_PNH` new handler.
/// @return Must be `int`; 0 means "do not retry", moot since we terminate.
/// @note Only fires for allocations routed through the CRT new handler. A
///       default `operator new` failure throws std::bad_alloc and lands in
///       on_terminate() instead.
int on_new_failed(size_t) {
    report_synthetic(L"operator new failed", 0xE0000001);
    return 0;
}

} // namespace

// --- public API -------------------------------------------------------------

void protect_current_thread() {
    ULONG guarantee = 64 * 1024;  // room to reach dispatch() after an overflow
    SetThreadStackGuarantee(&guarantee);
}

void register_module(const wchar_t* display_name, void* module_base, const GitInfo& git) {
    modules::add(display_name, module_base, git);
}

void mark_module_unloaded(void* module_base) {
    modules::mark_unloaded(module_base);
}

bool install(const Config& cfg) {
    if (g.installed) return true;

    lstrcpynW(g.dump_dir,    cfg.dump_dir.c_str(),    kPathMax);
    lstrcpynW(g.mirror_dir,  cfg.mirror_dir.c_str(),  kPathMax);
    lstrcpynW(g.app_name,    cfg.app_name.c_str(),    128);
    lstrcpynW(g.app_version, cfg.app_version.c_str(), 64);
    lstrcpynW(g.log_file,    cfg.log_file.c_str(),    kPathMax);

    g.break_into_debugger = cfg.break_into_debugger;

    CreateDirectoryW(g.dump_dir, nullptr);

    g.work_ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.work_done  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g.work_ready && g.work_done) {
        DWORD tid = 0;
        g.writer_thread = CreateThread(nullptr, 512 * 1024, writer_main, nullptr, 0, &tid);
    }

    protect_current_thread();

    // Keep unattended machines from blocking on a modal error box.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    SetUnhandledExceptionFilter(seh_filter);

    std::set_terminate(on_terminate);
    _set_purecall_handler(on_purecall);
    _set_invalid_parameter_handler(on_invalid_parameter);
    _set_new_handler(on_new_failed);
    std::signal(SIGABRT, on_signal);
    std::signal(SIGSEGV, on_signal);
    std::signal(SIGILL,  on_signal);
    std::signal(SIGFPE,  on_signal);

    g.installed = true;
    modules::add(g.app_name, GetModuleHandleW(nullptr), cfg.git);
    return true;
}

std::wstring write_report_now(std::wstring_view reason) {
    // Non-fatal path: report, then release the guard so the app keeps running.
    // If a real crash is already being reported, do not race it.
    if (InterlockedCompareExchange(&g.handling, 1, 0) != 0)
        return {};

    capture_context(nullptr, std::wstring(reason).c_str());  // also builds g.stem
    run_writer();

    std::wstring id = g.stem;  // the id that actually names the files on disk

    if (g.pending.thread) {
        CloseHandle(g.pending.thread);
        g.pending.thread = nullptr;
    }
    InterlockedExchange(&g.handling, 0);
    return id;
}

} // namespace crash
