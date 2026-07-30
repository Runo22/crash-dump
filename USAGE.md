# crash_handler — Capabilities & Usage

A compact, offline crash reporter for Windows / MSVC. On any fatal fault it
writes two files side by side — a symbolized `.log` and a minidump `.dmp` — from
a **dedicated thread with its own stack**, so even a stack overflow is
reportable. Nothing on the report path uses the CRT (no allocation, no locks, no
buffered I/O), because after heap corruption the CRT cannot be trusted.

> This document is the quick, example-driven guide. See `README.md` for the
> build-flag rationale, the report format walkthrough, and the design notes.

---

## 1. Capabilities at a glance

| Capability | What it gives you |
|---|---|
| **Two artifacts per crash** | `<stem>.log` (read first — usually enough) and `<stem>.dmp` (open in WinDbg when the log is not enough) |
| **Stack-overflow safe** | The report is produced on a separate writer thread with its own stack; the faulting thread only signals and waits |
| **CRT-free report path** | `CreateFileW` / `WriteFile` / `wvsprintfA` + static buffers — survives heap corruption |
| **Offline symbolization** | `module+RVA` is always resolved from the module table; function name + source line are a bonus when a PDB is reachable |
| **Module & git identity** | Each binary registers its name, commit hash, and dirty flag; the report shows exactly which build faulted |
| **Unloaded-plugin detection** | A dangling callback into an unloaded module is tagged `[UNLOADED]` — the diagnosis for a whole class of bugs |
| **Best-effort LAN mirror** | An optional second copy to a network share, written only after the local copy succeeds |
| **Voluntary reports** | `write_report_now()` captures state for hangs/watchdogs without crashing |
| **Bounded, non-hanging** | The faulting thread waits on the writer with a hard timeout; the registry readers use a bounded lock so nothing spins forever |

### What gets caught

Everything funnels through one path (`dispatch`):

| Hook | Catches |
|---|---|
| `SetUnhandledExceptionFilter` | Access violation, stack overflow, divide by zero, illegal instruction |
| `std::set_terminate` | Unhandled C++ throw, `noexcept` violation, throw during unwind |
| `_set_purecall_handler` | Pure virtual call from a constructor/destructor |
| `_set_invalid_parameter_handler` | CRT contract violation (`sprintf_s` overflow and friends) |
| `_set_new_handler` | CRT-routed `operator new` failure |
| `signal()` | `SIGABRT` (so `assert`, `std::abort`), `SIGSEGV`, `SIGILL`, `SIGFPE` |

---

## 2. Public API

The entire surface lives in `crash_handler.hpp`:

```cpp
namespace crash {

struct GitInfo {
    const char* hash    = "";     // short or full sha
    const char* message = "";     // first line only
    bool        dirty   = false;  // uncommitted changes at build time
};

struct Config {
    std::wstring dump_dir    = L"crashes";  // local report dir (created if missing)
    std::wstring mirror_dir;                // optional LAN copy; empty disables it
    std::wstring app_name    = L"app";
    std::wstring app_version = L"0.0.0";    // must match the archived PDBs
    GitInfo      git;                       // identity of the exe itself
    std::wstring log_file;                  // optional: app log path, recorded in the header
};

bool         install(const Config& cfg);                 // call once, first thing in main()
void         protect_current_thread();                   // call from each long-lived worker
void         register_module(const wchar_t* display_name,// call right after LoadLibrary
                             void* module_base, const GitInfo& git);
void         mark_module_unloaded(void* module_base);    // call right before FreeLibrary
std::wstring write_report_now(std::wstring_view reason);  // voluntary report; returns the stem id

} // namespace crash
```

---

## 3. Usage examples

### 3.1 Minimal — get crashes to disk

```cpp
#include "crash_handler.hpp"

int main(int argc, char** argv) {
    crash::Config cfg;
    cfg.app_name = L"myapp";
    crash::install(cfg);          // BEFORE anything else can fault

    return run();
}
```

That is enough to get `crashes\myapp_0.0.0_<timestamp>_<pid>.log` + `.dmp`.

### 3.2 Full configuration — versioned, mirrored, paired with your log

```cpp
int main(int argc, char** argv) {
    crash::Config cfg;
    cfg.app_name    = L"sim";
    cfg.app_version = APP_VERSION_W;                    // injected by CMake
    cfg.dump_dir    = L"C:\\ProgramData\\sim\\crashes"; // local disk, always reachable
    cfg.mirror_dir  = L"\\\\lab-nas\\sim\\crashes";     // optional LAN copy
    cfg.log_file    = L"C:\\ProgramData\\sim\\logs\\sim.log";
    cfg.git         = { GIT_HASH, GIT_MESSAGE, GIT_DIRTY };

    crash::install(cfg);          // BEFORE Qt, BEFORE the first plugin load

    QApplication app(argc, argv);
    return app.exec();
}
```

### 3.3 Worker threads — keep stack-overflow reports working

`SetThreadStackGuarantee` is **per thread**. `install()` only covers the main
thread; every long-lived worker you spawn must opt in:

```cpp
void worker_main() {
    crash::protect_current_thread();   // reserve the stack needed to report an overflow
    for (;;) do_work();
}
```

### 3.4 Plugins — name the faulting build, catch dangling callbacks

```cpp
// On load:
HMODULE h = LoadLibraryW(path.c_str());
if (h) {
    crash::GitInfo gi{};
    using GitFn = const PluginGitInfo* (*)();
    if (auto fn = reinterpret_cast<GitFn>(GetProcAddress(h, "plugin_git_info"))) {
        const auto* pg = fn();
        gi = { pg->hash, pg->message, pg->dirty };
    }
    crash::register_module(L"radar_plugin", h, gi);   // registered at load time
}

// On unload:
crash::mark_module_unloaded(h);   // BEFORE FreeLibrary — the record is kept, not erased
FreeLibrary(h);
```

The handler never enters a DLL at crash time; reaching into a module that is
being unloaded while handling a fault turns one crash into two. That is why
identity is captured at load time and merely *flagged* on unload.

### 3.5 Watchdog / voluntary report — for hangs, which never crash

```cpp
if (frames_stalled_for > std::chrono::seconds(5)) {
    std::wstring id = crash::write_report_now(L"watchdog: no frame for 5s");
    // reports current state, then KEEPS RUNNING. `id` names the .dmp/.log pair.
    log_error("captured state as {}", id);
}
```

`write_report_now` returns the shared timestamp stem of the two files it wrote,
or an empty string if a real crash was already being reported.

---

## 4. Build integration (CMake)

```cmake
# Link the static library from the exe only. Plugins do not link it; the module
# handler registers them on their behalf.
add_subdirectory(src/crash)
target_link_libraries(sim PRIVATE crash_handler)
```

Symbols must be enabled for **every** target — the exe, every static library,
and every plugin DLL — or the report degrades to bare hex. Set the flags once at
the top level so no target can forget:

```cmake
# Top-level CMakeLists.txt, BEFORE any add_subdirectory():
add_compile_options(/Z7 $<$<NOT:$<CONFIG:Debug>>:/Oy->)
add_link_options(/DEBUG:FULL /OPT:REF /PDBALTPATH:%_PDB%)
```

`/Z7` (not `/Zi`) avoids the `mspdbsrv` contention that bites parallel Ninja
builds; `/Oy-` keeps frame pointers so the stack walk does not truncate early.
See `README.md §6` for the full rationale, and archive the matching `.pdb` per
build.

---

## 5. Reading a report (30-second version)

```
=== CRASH REPORT ===
app       : sim 1.4.2+a3f91c2e08
exception : 0xC0000005  ACCESS_VIOLATION
fault at  : 0x00007FFA1C2E4410  radar_plugin+0x00024410
access    : read at 0x0000000000000018        <- tiny address = null + member offset

--- call stack (faulting thread) ---
#00  ...  radar_plugin+0x00024410  RadarSystem::tick+0x8C  [radar_system.cpp:212]

--- modules in the stack ---
radar_plugin   b7c1d99012  *** DIRTY BUILD ***  [UNLOADED]
```

1. **`access:`** — a tiny address means a null pointer plus a field offset.
2. **`#00`** — the function and source line where it happened.
3. **`[UNLOADED]`** — the real bug is whatever failed to tear down that plugin's
   callback, not the function that ran.
4. **`*** DIRTY BUILD ***`** — the binary is not exactly that commit; do not read
   the source at that sha and trust it.

Move to the `.dmp` in WinDbg when you need other threads (`~*k`), locals
(`dv /t /v`), or heap state (`!heap -s`).

---

## 6. Verify before you rely on it

Trigger each case and confirm you get a `.dmp` plus a `.log` with a resolved
stack. Do not skip the stack overflow — it is the only reason the dedicated
writer thread exists.

```cpp
*(volatile int*)nullptr = 1;    // access violation
std::vector<int>{}.at(99);      // unhandled throw -> std::terminate
std::abort();                   // SIGABRT
recurse_forever();              // stack overflow -> exercises the writer thread
```

Trigger one from inside a plugin too, and confirm the report shows that
plugin's name, commit hash, and dirty state.
