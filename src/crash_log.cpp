#include "crash_log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace vcr {
namespace {

constexpr std::size_t kPathBufferSize = 1024;
constexpr std::size_t kReportBufferSize = 16384;

struct CrashState {
    std::array<char, 128> app_name{"vcr"};
    std::array<char, 1024> command_line{};
    std::array<char, 1024> last_diagnostic{};
    std::atomic<const char*> stage{"startup"};
    std::atomic_flag diagnostic_lock = ATOMIC_FLAG_INIT;
    std::atomic_flag write_guard = ATOMIC_FLAG_INIT;
    std::atomic<bool> handlers_installed{false};

#ifdef _WIN32
    std::array<wchar_t, kPathBufferSize> exe_path_w{};
    std::array<wchar_t, kPathBufferSize> primary_log_w{};
    std::array<wchar_t, kPathBufferSize> cwd_log_w{};
    std::array<wchar_t, kPathBufferSize> temp_log_w{};
#else
    std::array<char, kPathBufferSize> primary_log{};
    std::array<char, kPathBufferSize> cwd_log{};
#endif
};

CrashState& crash_state() noexcept {
    static CrashState state;
    return state;
}

void copy_cstr(char* dst, const std::size_t dst_size, const std::string_view src) noexcept {
    if (dst_size == 0) {
        return;
    }
    const std::size_t count = (std::min)(dst_size - 1, src.size());
    if (count > 0) {
        std::memcpy(dst, src.data(), count);
    }
    dst[count] = '\0';
}

struct ReportWriter {
    char* buffer = nullptr;
    std::size_t capacity = 0;
    std::size_t length = 0;

    void append_sv(const std::string_view text) noexcept {
        if (buffer == nullptr || length >= capacity) {
            return;
        }
        const std::size_t remaining = capacity - 1 - length;
        const std::size_t count = (std::min)(remaining, text.size());
        if (count > 0) {
            std::memcpy(buffer + length, text.data(), count);
            length += count;
        }
        buffer[length] = '\0';
    }

    template <typename... Args>
    void append_fmt(const char* fmt, Args... args) noexcept {
        if (buffer == nullptr || length >= capacity) {
            return;
        }
        const std::size_t remaining = capacity - length;
        const int written = std::snprintf(buffer + length, remaining, fmt, args...);
        if (written > 0) {
            const std::size_t advance =
                (std::min)(remaining - 1, static_cast<std::size_t>(written));
            length += advance;
        }
    }
};

#ifdef _WIN32
void copy_wstr(wchar_t* dst, const std::size_t dst_size, const wchar_t* src) noexcept {
    if (dst_size == 0 || src == nullptr) {
        return;
    }
    std::size_t i = 0;
    while (i + 1 < dst_size && src[i] != L'\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = L'\0';
}

void append_wstr(wchar_t* dst, const std::size_t dst_size, const wchar_t* src) noexcept {
    if (dst_size == 0 || src == nullptr) {
        return;
    }
    std::size_t len = 0;
    while (len < dst_size && dst[len] != L'\0') {
        ++len;
    }
    std::size_t i = 0;
    while (len + 1 < dst_size && src[i] != L'\0') {
        dst[len++] = src[i++];
    }
    dst[len] = L'\0';
}

void wide_to_utf8(
    const wchar_t* src,
    char* dst,
    const std::size_t dst_size) noexcept {
    if (dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (src == nullptr || src[0] == L'\0') {
        return;
    }
    const int written = WideCharToMultiByte(
        CP_UTF8, 0, src, -1, dst, static_cast<int>(dst_size), nullptr, nullptr);
    if (written <= 0) {
        copy_cstr(dst, dst_size, "<utf8-conversion-failed>");
    }
}

void build_log_filename_w(
    const wchar_t* dir,
    const wchar_t* stem,
    wchar_t* out,
    const std::size_t out_size) noexcept {
    if (out_size == 0) {
        return;
    }
    out[0] = L'\0';
    if (dir == nullptr || dir[0] == L'\0') {
        return;
    }
    copy_wstr(out, out_size, dir);
    std::size_t len = std::wcslen(out);
    if (len > 0 && out[len - 1] != L'\\' && out[len - 1] != L'/') {
        append_wstr(out, out_size, L"\\");
    }
    if (stem != nullptr && stem[0] != L'\0') {
        append_wstr(out, out_size, stem);
        append_wstr(out, out_size, L"_crash.log");
    } else {
        append_wstr(out, out_size, L"crash.log");
    }
}

void resolve_paths_windows(CrashState& state) noexcept {
    const DWORD exe_len = GetModuleFileNameW(
        nullptr, state.exe_path_w.data(), static_cast<DWORD>(state.exe_path_w.size()));
    std::array<wchar_t, kPathBufferSize> exe_dir{};
    std::array<wchar_t, 256> exe_stem{};
    if (exe_len > 0 && exe_len < state.exe_path_w.size()) {
        copy_wstr(exe_dir.data(), exe_dir.size(), state.exe_path_w.data());
        wchar_t* last_slash = nullptr;
        for (wchar_t* p = exe_dir.data(); *p != L'\0'; ++p) {
            if (*p == L'\\' || *p == L'/') {
                last_slash = p;
            }
        }
        const wchar_t* filename = exe_dir.data();
        if (last_slash != nullptr) {
            *last_slash = L'\0';
            filename = last_slash + 1;
        }
        copy_wstr(exe_stem.data(), exe_stem.size(), filename);
        wchar_t* last_dot = nullptr;
        for (wchar_t* p = exe_stem.data(); *p != L'\0'; ++p) {
            if (*p == L'.') {
                last_dot = p;
            }
        }
        if (last_dot != nullptr) {
            *last_dot = L'\0';
        }
        build_log_filename_w(
            exe_dir.data(), exe_stem.data(),
            state.primary_log_w.data(), state.primary_log_w.size());
    }

    std::array<wchar_t, kPathBufferSize> cwd{};
    const DWORD cwd_len = GetCurrentDirectoryW(static_cast<DWORD>(cwd.size()), cwd.data());
    if (cwd_len > 0 && cwd_len < cwd.size()) {
        build_log_filename_w(
            cwd.data(), exe_stem.data(),
            state.cwd_log_w.data(), state.cwd_log_w.size());
    }

    std::array<wchar_t, kPathBufferSize> temp_dir{};
    const DWORD temp_len = GetTempPathW(static_cast<DWORD>(temp_dir.size()), temp_dir.data());
    if (temp_len > 0 && temp_len < temp_dir.size()) {
        build_log_filename_w(
            temp_dir.data(), exe_stem.data(),
            state.temp_log_w.data(), state.temp_log_w.size());
    }
}

bool write_file_windows(const wchar_t* path, const char* data, const std::size_t size) noexcept {
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }
    HANDLE file = CreateFileW(
        path,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
    return ok != FALSE && written == static_cast<DWORD>(size);
}

const char* seh_exception_name(const DWORD code) noexcept {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND: return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT: return "EXCEPTION_FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW: return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK: return "EXCEPTION_FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW: return "EXCEPTION_FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION: return "EXCEPTION_INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP: return "EXCEPTION_SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    case 0xE06D7363UL: return "MSVC_CXX_EXCEPTION";
    default: return "UNKNOWN_SEH_EXCEPTION";
    }
}

void try_extract_msvc_cxx_exception(
    const EXCEPTION_POINTERS* info,
    char* out_type,
    const std::size_t out_type_size,
    char* out_what,
    const std::size_t out_what_size) noexcept {
    out_type[0] = '\0';
    out_what[0] = '\0';
    if (info == nullptr || info->ExceptionRecord == nullptr) {
        return;
    }
    const auto* rec = info->ExceptionRecord;
    if (rec->ExceptionCode != 0xE06D7363UL || rec->NumberParameters < 4 ||
        rec->ExceptionInformation[0] != 0x19930520ULL) {
        return;
    }
    const auto obj_addr = static_cast<std::uintptr_t>(rec->ExceptionInformation[1]);
    const auto throw_info_addr = static_cast<std::uintptr_t>(rec->ExceptionInformation[2]);
    const auto image_base = static_cast<std::uintptr_t>(rec->ExceptionInformation[3]);
    if (obj_addr == 0 || throw_info_addr == 0 || image_base == 0) {
        return;
    }

    __try {
        struct MsvcTypeDescriptor {
            const void* pVFTable;
            void* spare;
            char name[1];
        };
        struct MsvcPMD {
            int mdisp;
            int pdisp;
            int vdisp;
        };
        struct MsvcCatchableType {
            unsigned int properties;
            unsigned int pType;
            MsvcPMD thisDisplacement;
            int sizeOrOffset;
            unsigned int copyFunction;
        };
        struct MsvcCatchableTypeArray {
            int nCatchableTypes;
            unsigned int arrayOfCatchableTypes[1];
        };
        struct MsvcThrowInfo {
            unsigned int attributes;
            unsigned int pmfnUnwind;
            unsigned int pForwardCompat;
            unsigned int pCatchableTypeArray;
        };

        const auto* throw_info =
            reinterpret_cast<const MsvcThrowInfo*>(throw_info_addr);
        if (throw_info->pCatchableTypeArray == 0) {
            return;
        }
        const auto* type_array = reinterpret_cast<const MsvcCatchableTypeArray*>(
            image_base + throw_info->pCatchableTypeArray);
        const int count = type_array->nCatchableTypes;
        if (count <= 0 || count > 32) {
            return;
        }
        for (int i = 0; i < count; ++i) {
            const auto* ct = reinterpret_cast<const MsvcCatchableType*>(
                image_base + type_array->arrayOfCatchableTypes[i]);
            const auto* td = reinterpret_cast<const MsvcTypeDescriptor*>(
                image_base + ct->pType);
            if (i == 0 && td->name[0] != '\0') {
                copy_cstr(out_type, out_type_size, td->name);
            }
            if (std::strcmp(td->name, ".?AVexception@std@@") == 0) {
                const auto* ex = reinterpret_cast<const std::exception*>(
                    obj_addr + static_cast<std::ptrdiff_t>(ct->thisDisplacement.mdisp));
                const char* what_str = ex->what();
                if (what_str != nullptr) {
                    copy_cstr(out_what, out_what_size, what_str);
                }
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void append_address_module(ReportWriter& writer, const void* address) noexcept {
    HMODULE module = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(address),
            &module) != FALSE &&
        module != nullptr) {
        std::array<char, MAX_PATH> module_path{};
        const DWORD len = GetModuleFileNameA(
            module, module_path.data(), static_cast<DWORD>(module_path.size()));
        const char* module_name = module_path.data();
        if (len > 0) {
            for (const char* p = module_path.data(); *p != '\0'; ++p) {
                if (*p == '\\' || *p == '/') {
                    module_name = p + 1;
                }
            }
        } else {
            module_name = "<unknown-module>";
        }
        const auto offset = static_cast<std::uintptr_t>(
            reinterpret_cast<const char*>(address) - reinterpret_cast<const char*>(module));
        writer.append_fmt(
            "0x%p (%s + 0x%llx)",
            address,
            module_name,
            static_cast<unsigned long long>(offset));
    } else {
        writer.append_fmt("0x%p (<unknown>)", address);
    }
}

void append_stack_trace_windows(ReportWriter& writer) noexcept {
    std::array<void*, 32> frames{};
    const USHORT count = RtlCaptureStackBackTrace(
        1, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
    if (count == 0) {
        return;
    }
    writer.append_sv("StackTrace:\n");
    for (USHORT i = 0; i < count; ++i) {
        writer.append_fmt("  [%02u] ", static_cast<unsigned>(i));
        append_address_module(writer, frames[i]);
        writer.append_sv("\n");
    }
}
#endif

void format_timestamp(ReportWriter& writer) noexcept {
    const std::time_t now = std::time(nullptr);
    std::tm local_tm{};
#ifdef _WIN32
    if (localtime_s(&local_tm, &now) == 0) {
#else
    if (localtime_r(&now, &local_tm) != nullptr) {
#endif
        writer.append_fmt(
            "%04d-%02d-%02d %02d:%02d:%02d",
            local_tm.tm_year + 1900,
            local_tm.tm_mon + 1,
            local_tm.tm_mday,
            local_tm.tm_hour,
            local_tm.tm_min,
            local_tm.tm_sec);
    } else {
        writer.append_sv("<unknown>");
    }
}

std::filesystem::path write_crash_log_impl(
    const std::string_view category,
    const std::string_view message,
    const void* seh_pointers_void) noexcept {
    auto& state = crash_state();
    if (state.write_guard.test_and_set(std::memory_order_acq_rel)) {
        return {};
    }

    std::array<char, kReportBufferSize> report{};
    ReportWriter writer{report.data(), report.size(), 0};

    writer.append_sv("=== VCR Crash / Fatal Error Log ===\n");
    writer.append_sv("Timestamp: ");
    format_timestamp(writer);
    writer.append_sv("\n");
    writer.append_fmt("Application: %s\n", state.app_name.data());

#ifdef _WIN32
    std::array<char, kPathBufferSize> utf8_buf{};
    wide_to_utf8(state.exe_path_w.data(), utf8_buf.data(), utf8_buf.size());
    writer.append_fmt("Executable: %s\n", utf8_buf.data());
    std::array<wchar_t, kPathBufferSize> cwd_w{};
    if (GetCurrentDirectoryW(static_cast<DWORD>(cwd_w.size()), cwd_w.data()) > 0) {
        wide_to_utf8(cwd_w.data(), utf8_buf.data(), utf8_buf.size());
        writer.append_fmt("WorkingDir: %s\n", utf8_buf.data());
    }
#endif

    if (state.command_line[0] != '\0') {
        writer.append_fmt("CommandLine: %s\n", state.command_line.data());
    }
    const char* stage = state.stage.load(std::memory_order_acquire);
    writer.append_fmt("Stage: %s\n", stage != nullptr ? stage : "<unknown>");
    writer.append_sv("Category: ");
    writer.append_sv(category.empty() ? std::string_view("Fatal Error") : category);
    writer.append_sv("\n");
    writer.append_sv("Message: ");
    writer.append_sv(message.empty() ? std::string_view("<none>") : message);
    writer.append_sv("\n");

    if (!state.diagnostic_lock.test_and_set(std::memory_order_acquire)) {
        if (state.last_diagnostic[0] != '\0' &&
            message != std::string_view(state.last_diagnostic.data())) {
            writer.append_fmt("LastDiagnostic: %s\n", state.last_diagnostic.data());
        }
        state.diagnostic_lock.clear(std::memory_order_release);
    }

#ifdef _WIN32
    if (seh_pointers_void != nullptr) {
        const auto* info = static_cast<const EXCEPTION_POINTERS*>(seh_pointers_void);
        if (info->ExceptionRecord != nullptr) {
            const DWORD code = info->ExceptionRecord->ExceptionCode;
            writer.append_fmt(
                "ExceptionCode: 0x%08lX (%s)\n",
                static_cast<unsigned long>(code),
                seh_exception_name(code));
            writer.append_sv("ExceptionAddress: ");
            append_address_module(writer, info->ExceptionRecord->ExceptionAddress);
            writer.append_sv("\n");
            if (code == 0xE06D7363UL) {
                std::array<char, 256> cxx_type{};
                std::array<char, 1024> cxx_what{};
                try_extract_msvc_cxx_exception(
                    info,
                    cxx_type.data(), cxx_type.size(),
                    cxx_what.data(), cxx_what.size());
                if (cxx_type[0] != '\0') {
                    writer.append_fmt("CxxExceptionType: %s\n", cxx_type.data());
                }
                if (cxx_what[0] != '\0') {
                    writer.append_fmt("CxxExceptionWhat: %s\n", cxx_what.data());
                }
            }
        }
    }
    append_stack_trace_windows(writer);

    std::filesystem::path written_path{};
    if (write_file_windows(state.primary_log_w.data(), report.data(), writer.length)) {
        written_path = std::filesystem::path(state.primary_log_w.data());
    } else if (write_file_windows(state.cwd_log_w.data(), report.data(), writer.length)) {
        written_path = std::filesystem::path(state.cwd_log_w.data());
    } else if (write_file_windows(state.temp_log_w.data(), report.data(), writer.length)) {
        written_path = std::filesystem::path(state.temp_log_w.data());
    }
    state.write_guard.clear(std::memory_order_release);
    return written_path;
#else
    (void)seh_pointers_void;
    std::filesystem::path written_path{};
    const char* candidates[2] = {state.primary_log.data(), state.cwd_log.data()};
    for (const char* candidate : candidates) {
        if (candidate != nullptr && candidate[0] != '\0') {
            if (std::FILE* f = std::fopen(candidate, "wb")) {
                std::fwrite(report.data(), 1, writer.length, f);
                std::fclose(f);
                written_path = std::filesystem::path(candidate);
                break;
            }
        }
    }
    state.write_guard.clear(std::memory_order_release);
    return written_path;
#endif
}

[[noreturn]] void terminate_handler() noexcept {
    std::string_view message = "std::terminate called";
    std::array<char, 512> msg_buf{};
    if (const auto current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& ex) {
            copy_cstr(msg_buf.data(), msg_buf.size(), ex.what());
            message = msg_buf.data();
        } catch (...) {
            message = "std::terminate called with unknown active exception";
        }
    }
    write_crash_log_impl("std::terminate", message, nullptr);
    std::abort();
}

void signal_handler(const int sig) noexcept {
    const char* name = "UNKNOWN_SIGNAL";
    switch (sig) {
    case SIGABRT: name = "SIGABRT"; break;
    case SIGSEGV: name = "SIGSEGV"; break;
    case SIGILL: name = "SIGILL"; break;
    case SIGFPE: name = "SIGFPE"; break;
    default: break;
    }
    write_crash_log_impl("Signal", name, nullptr);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

#ifdef _WIN32
LONG WINAPI seh_exception_filter(EXCEPTION_POINTERS* exception_info) noexcept {
    write_crash_log_impl(
        "Unhandled SEH Exception",
        "Native hardware/OS exception",
        exception_info);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

} // namespace

void install_crash_handlers(
    const std::string_view app_name,
    const int argc,
    char** argv) noexcept {
    auto& state = crash_state();
    copy_cstr(state.app_name.data(), state.app_name.size(), app_name);

    ReportWriter cmd_writer{state.command_line.data(), state.command_line.size(), 0};
    for (int i = 0; i < argc && argv != nullptr; ++i) {
        if (argv[i] == nullptr) {
            continue;
        }
        if (i > 0) {
            cmd_writer.append_sv(" ");
        }
        cmd_writer.append_sv(argv[i]);
    }

#ifdef _WIN32
    resolve_paths_windows(state);
#else
    copy_cstr(state.primary_log.data(), state.primary_log.size(), "crash.log");
    copy_cstr(state.cwd_log.data(), state.cwd_log.size(), "crash.log");
#endif

    if (!state.handlers_installed.exchange(true, std::memory_order_acq_rel)) {
        std::set_terminate(terminate_handler);
        std::signal(SIGABRT, signal_handler);
        std::signal(SIGSEGV, signal_handler);
        std::signal(SIGILL, signal_handler);
        std::signal(SIGFPE, signal_handler);
#ifdef _WIN32
        SetUnhandledExceptionFilter(seh_exception_filter);
#endif
    }
}

void set_crash_stage(const char* stage) noexcept {
    crash_state().stage.store(stage != nullptr ? stage : "<null>", std::memory_order_release);
}

void record_crash_diagnostic(const std::string_view message) noexcept {
    auto& state = crash_state();
    while (state.diagnostic_lock.test_and_set(std::memory_order_acquire)) {
    }
    copy_cstr(state.last_diagnostic.data(), state.last_diagnostic.size(), message);
    state.diagnostic_lock.clear(std::memory_order_release);
}

std::filesystem::path crash_log_path() {
    const auto& state = crash_state();
#ifdef _WIN32
    if (state.primary_log_w[0] != L'\0') {
        return std::filesystem::path(state.primary_log_w.data());
    }
    if (state.cwd_log_w[0] != L'\0') {
        return std::filesystem::path(state.cwd_log_w.data());
    }
    if (state.temp_log_w[0] != L'\0') {
        return std::filesystem::path(state.temp_log_w.data());
    }
    return std::filesystem::path("crash.log");
#else
    return std::filesystem::path(
        state.primary_log[0] != '\0' ? state.primary_log.data() : "crash.log");
#endif
}

std::filesystem::path write_crash_log(
    const std::string_view category,
    const std::string_view message) noexcept {
    return write_crash_log_impl(category, message, nullptr);
}

} // namespace vcr
