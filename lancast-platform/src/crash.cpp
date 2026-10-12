#include "lancast/platform/crash.hpp"

#include "win.hpp"

#include <dbghelp.h>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <string>

#include "lancast/core/log.hpp"
#include "lancast/core/version.hpp"

namespace lancast::crash {
namespace {

constexpr DWORD kAbortCode = 0xE0000001;  // Excepción propia para abort/terminate/purecall/parámetro inválido.
constexpr std::size_t kKeep = 10;

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

struct State {
    std::filesystem::path dir;
    std::wstring role;
    MiniDumpWriteDumpFn write = nullptr;
    HANDLE request = nullptr;  // El hilo que falla pide el volcado...
    HANDLE done = nullptr;     // ...y espera a que esté escrito.
    EXCEPTION_POINTERS* pointers = nullptr;
    DWORD thread_id = 0;
    std::filesystem::path last_file;
    volatile LONG busy = 0;
};
State g;

void prune() {
    auto files = list(g.dir);
    std::error_code ec;
    for (std::size_t i = kKeep; i < files.size(); ++i) std::filesystem::remove(files[i], ec);
}

void write_dump() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    const std::wstring name = std::format(L"LanCast-{}-{}-{:04}{:02}{:02}-{:02}{:02}{:02}-{}.dmp", platform::detail::to_wide(kAppVersion),
                                          g.role, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
    g.last_file = g.dir / name;
    HANDLE file = CreateFileW(g.last_file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = g.thread_id;
    info.ExceptionPointers = g.pointers;
    info.ClientPointers = FALSE;
    // Pilas, hilos, módulos y la memoria a la que apuntan: suficiente para ver dónde y por qué falló (pocos MB).
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
                                                 MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    g.write(GetCurrentProcess(), GetCurrentProcessId(), file, type, g.pointers != nullptr ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
}

DWORD WINAPI dump_thread(void*) {
    WaitForSingleObject(g.request, INFINITE);
    write_dump();
    prune();
    SetEvent(g.done);
    return 0;
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* pointers) {
    if (InterlockedExchange(&g.busy, 1) != 0) {  // Otro hilo ya está escribiendo el volcado.
        Sleep(INFINITE);
    }
    g.pointers = pointers;
    g.thread_id = GetCurrentThreadId();
    SetEvent(g.request);
    WaitForSingleObject(g.done, 30000);
    const DWORD code = pointers != nullptr && pointers->ExceptionRecord != nullptr ? pointers->ExceptionRecord->ExceptionCode : 0;
    log::error("Cierre inesperado (excepción 0x{:08X}). Volcado: {}", code, platform::detail::to_utf8(g.last_file.wstring()));
    return EXCEPTION_EXECUTE_HANDLER;  // Termina el proceso.
}

[[noreturn]] void raise_abort() {
    RaiseException(kAbortCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    std::_Exit(3);
}

void on_signal(int) { raise_abort(); }
void on_purecall() { raise_abort(); }
void on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { raise_abort(); }

}  // namespace

void install(const std::filesystem::path& dir, std::string_view role) {
    if (g.request != nullptr) return;
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dbghelp == nullptr) return;
    g.write = reinterpret_cast<MiniDumpWriteDumpFn>(reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
    if (g.write == nullptr) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    g.dir = dir;
    g.role = platform::detail::to_wide(role);
    g.request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // Hilo con su propia pila: escribe el volcado aunque el hilo que falló no tenga pila libre.
    if (HANDLE t = CreateThread(nullptr, 256 * 1024, dump_thread, nullptr, 0, nullptr); t != nullptr) CloseHandle(t);

    SetUnhandledExceptionFilter(on_unhandled);
    std::set_terminate([] { raise_abort(); });
    std::signal(SIGABRT, on_signal);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_purecall_handler(on_purecall);
    _set_invalid_parameter_handler(on_invalid_parameter);
    ULONG stack = 64 * 1024;  // Reserva para el filtro si el fallo fue un desbordamiento de pila.
    SetThreadStackGuarantee(&stack);
}

std::vector<std::filesystem::path> list(const std::filesystem::path& dir) {
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> found;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == L".dmp") found.emplace_back(e.last_write_time(ec), e.path());
    }
    std::ranges::sort(found, [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::filesystem::path> out;
    for (auto& [time, path] : found) out.push_back(std::move(path));
    return out;
}

std::optional<std::filesystem::path> unseen(const std::filesystem::path& dir) {
    const auto files = list(dir);
    if (files.empty()) return std::nullopt;
    std::wifstream f(dir / L"visto.txt");
    std::wstring seen;
    std::getline(f, seen);
    if (seen == files.front().filename().wstring()) return std::nullopt;
    return files.front();
}

void mark_seen(const std::filesystem::path& dir) {
    const auto files = list(dir);
    if (files.empty()) return;
    std::wofstream f(dir / L"visto.txt", std::ios::trunc);
    f << files.front().filename().wstring();
}

void crash_for_test() {
    volatile int* p = nullptr;
    *p = 42;
    std::_Exit(4);
}

}  // namespace lancast::crash
