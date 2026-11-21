#include <rsdkair/paths.hpp>

#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace rsdkair {

namespace fs = std::filesystem;

fs::path path_from_utf8(const std::string &utf8)
{
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string path_to_utf8(const fs::path &path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

fs::path self_executable()
{
#if defined(__ANDROID__)
    return {};
#elif defined(_WIN32)
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size())
            return fs::path(std::wstring(buffer.data(), length));
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    std::error_code ec;
    const fs::path resolved = fs::canonical(buffer.data(), ec);
    return ec ? fs::path(buffer.data()) : resolved;
#else
    std::error_code ec;
    const fs::path resolved = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : resolved;
#endif
}

std::string module_file_name(std::string_view module)
{
#if defined(__ANDROID__)
    return "lib" + std::string(module) + ".so";
#elif defined(_WIN32)
    return std::string(module) + ".dll";
#else
    return std::string(module) + ".so";  // CMake MODULE libraries use .so on macOS too
#endif
}

fs::path module_path(std::string_view module)
{
#if defined(__ANDROID__)
    return fs::path(module_file_name(module));
#else
    return self_executable().parent_path() / "engines" / module_file_name(module);
#endif
}

} // namespace rsdkair
