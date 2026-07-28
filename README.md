# crash_handler

Crash reporting for an offline Windows / MSVC / Ninja build. Every crash writes
two files side by side:

| File | Contents | When you read it |
|---|---|---|
| `<stem>.log` | Symbolized call stack, faulting module + commit, module table | **First.** Usually the diagnosis ends here |
| `<stem>.dmp` | Minidump: all thread stacks, referenced heap, handles | When the log is not enough. Opened in WinDbg |

No server, no service, no external dependency. Nothing on the report path uses
the CRT — no allocation, no locks, no buffered I/O — because after heap
corruption the CRT is not trustworthy.

---

## 1. Layout

| File | Role |
|---|---|
| `crash_handler.hpp` | **The only public header.** Everything you call lives here |
| `crash_handler.cpp` | Hook installation, writer thread, public API |
| `crash_io.*` | CRT-free writers (`WriteFile` + `wvsprintfA`) |
| `crash_modules.*` | Module registry, git identity, address → name resolution |
| `crash_symbols.*` | dbghelp stack walking and symbolization |
| `crash_report.*` | Text log and minidump generation |

Only `crash_handler.hpp` is meant to be included; the rest are internal.

---

## 2. Setup in five minutes

### 2.1 Add the library

```cmake
add_subdirectory(src/crash)
target_link_libraries(sim PRIVATE crash_handler)
```

### 2.2 Enable symbols — for **every** target

Skip this and the report becomes a wall of hex. See §6 for why.

```cmake
# Top-level CMakeLists.txt, BEFORE any add_subdirectory():
add_compile_options(/Z7 $<$<NOT:$<CONFIG:Debug>>:/Oy->)
add_link_options(/DEBUG:FULL /OPT:REF /PDBALTPATH:%_PDB%)
string(REPLACE "/Zi" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
string(REPLACE "/Zi" "" CMAKE_CXX_FLAGS_RELEASE        "${CMAKE_CXX_FLAGS_RELEASE}")
```

### 2.3 Entry point

```cpp
#include "crash_handler.hpp"

int main(int argc, char** argv) {
    crash::Config cfg;
    cfg.app_name    = L"sim";
    cfg.app_version = APP_VERSION_W;                   // injected by CMake
    cfg.dump_dir    = L"C:\\ProgramData\\sim\\crashes";
    cfg.mirror_dir  = L"\\\\lab-nas\\sim\\crashes";    // optional
    cfg.log_file    = L"C:\\ProgramData\\sim\\logs\\sim.log";
    cfg.git         = { GIT_HASH, GIT_MESSAGE, GIT_DIRTY };

    crash::install(cfg);        // BEFORE Qt, BEFORE the first plugin load

    QApplication app(argc, argv);
    ...
}
```

Call `install()` as early as possible. A crash before it is not captured.

### 2.4 Module handler

```cpp
HMODULE h = LoadLibraryW(path.c_str());
if (h) {
    using GitFn = const PluginGitInfo* (*)();
    crash::GitInfo gi{};
    if (auto fn = reinterpret_cast<GitFn>(GetProcAddress(h, "plugin_git_info"))) {
        const auto* pg = fn();
        gi = { pg->hash, pg->message, pg->dirty };
    }
    crash::register_module(name.c_str(), h, gi);
}
```

```cpp
// Unload:
crash::mark_module_unloaded(h);   // BEFORE FreeLibrary
FreeLibrary(h);
```

`register_module` is called **at load time**. The handler never enters a DLL at
crash time: reaching into a module that is being unloaded while handling a fault
turns one crash into two.

`mark_module_unloaded` marks the record instead of erasing it. When a dangling
callback into an unloaded plugin brings the process down, the report shows
`[UNLOADED]` — that single tag is the diagnosis for this whole class of bug.

### 2.5 Worker threads

```cpp
void worker_main() {
    crash::protect_current_thread();
    ...
}
```

`SetThreadStackGuarantee` is **per thread**. `install()` covers the main thread
only. A stack overflow on a thread you did not protect cannot be reported.

---

## 3. Reading a report

```
=== CRASH REPORT ===
time      : 2026-07-28 14:22:11
app       : sim 1.4.2+a3f91c2e08
pid / tid : 9184 / 4412
app log   : C:\ProgramData\sim\logs\sim.log
exception : 0xC0000005  ACCESS_VIOLATION
fault at  : 0x00007FFA1C2E4410  radar_plugin+0x00024410
access    : read at 0x0000000000000018
workingset: 2143 MB (peak 2390 MB)

--- call stack (faulting thread) ---
#00  0x00007FFA1C2E4410  radar_plugin+0x00024410  RadarSystem::tick+0x8C  [radar_system.cpp:212]
#01  0x00007FFA4419A0C0  flecs.dll+0x0001A0C0
#02  0x00007FF6A21B3340  sim+0x000A3340  SimLoop::frame+0x1F0  [sim_loop.cpp:88]

--- modules in the stack ---
radar_plugin             b7c1d99012  *** DIRTY BUILD ***  [UNLOADED]
     tune cfar thresholds
sim                      a3f91c2e08
     fix radar occlusion culling
```

Read it in this order:

1. **`access: read at 0x...18`** — a tiny address, so a null pointer plus a
   member offset. Something was null when a field was read.
2. **The `#00` line** — which function, which source line.
3. **`[UNLOADED]`** — the plugin was already unloaded, so this is a dangling
   callback. The real bug is not in `RadarSystem::tick`; it is in whatever
   failed to tear that callback down during unload.
4. **`*** DIRTY BUILD ***`** — do not check out this commit and read the source.
   The binary is not that commit.

### The two halves of a stack line

```
#00  0x00007FFA1C2E4410  radar_plugin+0x00024410   RadarSystem::tick+0x8C  [radar_system.cpp:212]
     └─ address ───────┘  └─ module + RVA ───────┘  └─ present if a PDB was reachable ───────────┘
```

The left half **never fails** — it is computed from the loaded module table. Even
with no PDB on the machine you can resolve it offline against
`radar_plugin.pdb`. The right half is a bonus.

---

## 4. When to move to the dump

The text log covers one thread and cannot show inlined frames. Switch to the
`.dmp` when:

| Symptom | WinDbg command |
|---|---|
| Stack looks nonsensical or empty | `!analyze -v` |
| The culprit seems to be another thread | `~*k` — all threads |
| Suspected unloaded module | `lmu` |
| You need local variable values | `dv /t /v` |
| Suspected heap corruption | `!heap -s` |

```
windbg -z sim_1.4.2+a3f91c2e08_20260728_142211_9184.dmp
.sympath \\lab-nas\symbols\1.4.2\RelWithDebInfo
.reload /f
!analyze -v
```

In a thread-heavy application `~*k` is the most valuable command: the thread
that took the fault is usually the symptom, and the cause sits elsewhere.

Name your threads so they show up in `~*`:

```cpp
SetThreadDescription(GetCurrentThread(), L"cigi-tx");
```

---

## 5. What gets caught

All of it funnels through one path (`dispatch`):

| Hook | Catches |
|---|---|
| `SetUnhandledExceptionFilter` | Access violation, stack overflow, divide by zero, illegal instruction |
| `std::set_terminate` | Unhandled C++ throw, `noexcept` violation, throw during unwind |
| `_set_purecall_handler` | Pure virtual call from a constructor or destructor |
| `_set_invalid_parameter_handler` | CRT contract violation (`sprintf_s` overflow and friends) |
| `signal()` | `SIGABRT` (so `assert`, `std::abort`), `SIGSEGV`, `SIGILL`, `SIGFPE` |

With only `SetUnhandledExceptionFilter` installed you would miss unhandled C++
exceptions and `abort()` — in practice the two most common cases.

### Watchdog reports (for hangs)

A deadlock never crashes, so it never produces a dump:

```cpp
if (frames_stalled_for > 5s)
    crash::write_report_now(L"watchdog: no frame for 5s");  // reports, keeps running
```

In a thread-heavy system you will reach for this as often as for crashes.

---

## 6. Symbols: every target, every configuration

`/Z7` and `/DEBUG` are needed for **every binary you build**:

- **Static libraries** (`core`, `util`): `/Z7` is a *compile* flag. If `core` is
  built without it, frames coming from `core` stay nameless in the exe, and no
  linker flag on the exe can recover them.
- **Plugin DLLs**: each produces its own PDB. Miss one and you get
  `radar_plugin+0x24410` with no function name, precisely where you needed it.
- **The exe**: obviously.

This is why a per-target call like `enable_symbols(sim)` is **not enough** — and,
worse, fails silently. Set the flags globally at the top level instead (§2.2);
then there is no target left to forget.

### Why `/Z7` and not `/Zi`

`/Zi` routes debug info through `mspdbsrv` into a single shared `.pdb` per
target. Ninja runs `cl.exe` with high parallelism, which is where the classic
`fatal error C1041: cannot open program database` comes from. `/Z7` embeds debug
info in each `.obj`; there is no shared file to contend for. The linker still
emits a proper `.pdb` from `/DEBUG`. **Code generation is unchanged.**

### Why `/Oy-`

It keeps frame pointers. Without it some frames are lost even on x64 and the
stack walk truncates early.

### Why no `/OPT:ICF`

It folds identical function bodies onto one address, so a stack can name the
wrong function. The exe size it saves is not worth that confusion.

### RelWithDebInfo or Release

Both use `/O2`. The only difference is `/Z7 /DEBUG`, i.e. link time and a `.pdb`
on disk. **Runtime speed is identical.** Ship the exe, archive the PDB — but it
must be the PDB from *that* build; another build's PDB will silently produce
wrong function names.

### Symbol archive (offline)

No symbol server needed; a folder per version is enough:

```cmake
add_custom_command(TARGET sim POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${SYMBOL_STORE}/${PROJECT_VERSION}/$<CONFIG>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_PDB_FILE:sim>" "${SYMBOL_STORE}/${PROJECT_VERSION}/$<CONFIG>/")
```

Point `.sympath` or `_NT_SYMBOL_PATH` straight at that folder.

---

## 7. Working alongside spdlog

The handler **never touches** your log file. `cfg.log_file` is recorded in the
report header purely so the two artifacts can be paired later.

What matters is your flush policy: lines still buffered in a sink are lost when
the process dies.

```cpp
spdlog::flush_on(spdlog::level::warn);
spdlog::flush_every(std::chrono::seconds(1));
```

There is deliberately **no** hook that flushes spdlog at crash time: the faulting
thread may hold that mutex, and the writer thread would deadlock. Keeping the
on-disk log continuously current is the right fix.

---

## 8. Design notes

**Why a separate writer thread.** Running the handler on the faulting thread
fails exactly when you need it most — after a stack overflow there is no usable
stack left. The report is produced on a dedicated thread with its own stack; the
faulting thread only signals an event and waits. This is the free 90% of
Crashpad's out-of-process design. The remaining risk (a heap corrupt enough that
in-process writing fails) can only be solved by a genuinely separate process,
which is not worth the cost in an offline or LAN setting.

**Why no CRT.** After heap corruption `new`, `std::string` and `fopen` are
unreliable. Everything uses static buffers plus
`CreateFileW`/`WriteFile`/`wvsprintfA`. The price: `wvsprintfA` is a kernel
formatter with no `%p` and no `%f`, hence `io::write_ptr()`.

**Why `TerminateProcess`.** Returning to the CRT from the SEH filter runs
`atexit` handlers and destructors over corrupt state, turning one clean report
into a second, confusing crash.

**Why the minidump is not `Full`.** `MiniDumpWithFullMemory` writes the entire
address space — several GB with an ECS world, Qt and texture caches resident, and
10+ seconds to write. The flag set used here captures the heap the stacks
*reference*, so locals and the objects they point at are readable while unrelated
buffers stay out of the file. Typically 30–150 MB.
`MiniDumpIgnoreInaccessibleMemory` matters too: without it a single bad page can
fail the whole dump and leave you with nothing.

---

## 9. Verification

Do not trust the setup until you have tried it. Trigger all four; each must
produce a `.dmp` plus a `.log` with a resolved stack:

```cpp
*(volatile int*)nullptr = 1;    // access violation
std::vector<int>{}.at(99);      // unhandled throw -> terminate
std::abort();                   // SIGABRT
recurse_forever();              // stack overflow -> the writer thread's exam
```

Trigger one from inside a plugin as well, and confirm the report shows that
plugin's name, commit hash and dirty state.

Do not skip the stack overflow case — it is the only reason the dedicated writer
thread exists.
