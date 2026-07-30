////////////////////////////////////////////////////////////////////////////////
// User Defined Headers
#include "crash_modules.hpp"
#include "crash_io.hpp"
////////////////////////////////////////////////////////////////////////////////
// Windows Headers
#include <psapi.h>
////////////////////////////////////////////////////////////////////////////////

#pragma comment(lib, "psapi.lib")

namespace crash::modules {
namespace {

constexpr size_t kMaxModules     = 128;
constexpr int    kLockSpinBudget = 100000;  ///< try_lock() gives up after this many spins.

/// @brief One registered binary: the exe or a plugin.
struct Record {
    wchar_t name[64]{};
    void*   base     = nullptr;
    size_t  size     = 0;
    char    hash[24]{};
    char    message[96]{};
    bool    dirty    = false;
    bool    unloaded = false;
    bool    used     = false;
};

Record        g_records[kMaxModules];
volatile LONG g_lock = 0;

/// @brief Spin lock. A mutex would take a CRT lock, which is unsafe here.
void lock()   { while (InterlockedCompareExchange(&g_lock, 1, 0) != 0) Sleep(0); }
void unlock() { InterlockedExchange(&g_lock, 0); }

/// @brief Bounded acquire for the report path. Returns false if the lock could
///        not be taken within a fixed spin budget; the caller then reads the
///        registry without it. This keeps a crash that happened while a thread
///        held the lock -- e.g. faulting inside add() on a bad display_name --
///        from spinning the writer thread forever and losing the whole report.
///        Records are fixed-size PODs, so an unlocked read yields garbled text
///        at worst, never a fault: a slightly wrong table beats no report.
bool try_lock() {
    for (int i = 0; i < kLockSpinBudget; ++i) {
        if (InterlockedCompareExchange(&g_lock, 1, 0) == 0) return true;
        Sleep(0);
    }
    return false;
}

bool covers(const Record& m, const void* addr) {
    const auto a = reinterpret_cast<uintptr_t>(addr);
    const auto b = reinterpret_cast<uintptr_t>(m.base);
    return m.used && a >= b && a < b + m.size;
}

/// @note Caller normally holds the lock; on the report path it may run
///       lock-free (see try_lock), which is a tolerated best-effort read.
const Record* find(const void* addr) {
    for (const auto& m : g_records)
        if (covers(m, addr)) return &m;
    return nullptr;
}

} // namespace

void add(const wchar_t* display_name, void* module_base, const GitInfo& git) {
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), static_cast<HMODULE>(module_base),
                         &mi, sizeof(mi));

    lock();
    for (auto& m : g_records) {
        if (m.used) continue;
        lstrcpynW(m.name, display_name, 64);
        m.base     = module_base;
        m.size     = mi.SizeOfImage;
        m.dirty    = git.dirty;
        m.unloaded = false;
        m.used     = true;
        io::copy_ascii(m.hash, sizeof(m.hash), git.hash);
        io::copy_ascii(m.message, sizeof(m.message), git.message);
        break;
    }
    unlock();
}

void mark_unloaded(void* module_base) {
    lock();
    for (auto& m : g_records)
        if (m.used && m.base == module_base) m.unloaded = true;
    unlock();
}

void describe(const void* addr, char* out, int cap, DWORD& out_rva) {
    out[0]  = '\0';
    out_rva = 0;

    const bool locked = try_lock();
    const Record* m = find(addr);
    if (m) {
        io::narrow(out, cap, m->name);
        out_rva = static_cast<DWORD>(reinterpret_cast<uintptr_t>(addr) -
                                     reinterpret_cast<uintptr_t>(m->base));
    }
    if (locked) unlock();
    if (m) return;

    // Not one of ours: ask the loader so Qt, drivers and system DLLs still
    // get a name instead of a bare address.
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &mod) && mod) {
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(mod, path, MAX_PATH)) {
            const wchar_t* leaf = path;
            for (const wchar_t* p = path; *p; ++p)
                if (*p == L'\\' || *p == L'/') leaf = p + 1;
            io::narrow(out, cap, leaf);
        }
        out_rva = static_cast<DWORD>(reinterpret_cast<uintptr_t>(addr) -
                                     reinterpret_cast<uintptr_t>(mod));
    } else {
        io::copy_ascii(out, static_cast<size_t>(cap), "?");
    }
}

void write_table(HANDLE file, void* const* frames, size_t frame_count) {
    char name[128];

    io::write_str(file, "\r\n--- modules in the stack ---\r\n");
    const bool locked = try_lock();
    for (const auto& m : g_records) {
        if (!m.used) continue;

        bool hit = false;
        for (size_t i = 0; i < frame_count; ++i)
            if (covers(m, frames[i])) { hit = true; break; }
        if (!hit) continue;

        io::narrow(name, sizeof(name), m.name);
        io::write_fmt(file, "%-24s %s%s%s\r\n     %s\r\n",
                      name,
                      m.hash[0] ? m.hash : "(no git info)",
                      m.dirty    ? "  *** DIRTY BUILD ***" : "",
                      m.unloaded ? "  [UNLOADED]" : "",
                      m.message);
    }

    io::write_str(file, "\r\n--- all registered modules ---\r\n");
    for (const auto& m : g_records) {
        if (!m.used) continue;
        io::narrow(name, sizeof(name), m.name);
        io::write_ptr(file, m.base);
        io::write_fmt(file, "  %-24s  %-12s%s%s\r\n",
                      name, m.hash[0] ? m.hash : "-",
                      m.dirty    ? "  DIRTY" : "",
                      m.unloaded ? "  UNLOADED" : "");
    }
    if (locked) unlock();
}

} // namespace crash::modules
