#include <rsdkair/module.hpp>

#include <rsdkair/paths.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace rsdkair {

std::unique_ptr<Module> Module::open(const std::filesystem::path &path, std::string &error)
{
#if defined(_WIN32)
    // Altered search path: the module's own dependencies resolve next to it and the exe.
    HMODULE handle = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!handle) {
        error = "cannot load " + path_to_utf8(path) + " (error " + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    return std::unique_ptr<Module>(new Module(reinterpret_cast<void *>(handle)));
#else
    // RTLD_LOCAL: nothing the module defines is visible to anything loaded later.
    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *reason = dlerror();
        error = "cannot load " + path_to_utf8(path) + ": " + (reason ? reason : "unknown error");
        return nullptr;
    }
    return std::unique_ptr<Module>(new Module(handle));
#endif
}

Module::~Module()
{
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
}

void *Module::symbol(const char *name) const
{
#if defined(_WIN32)
    return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(handle_), name));
#else
    return dlsym(handle_, name);
#endif
}

} // namespace rsdkair
