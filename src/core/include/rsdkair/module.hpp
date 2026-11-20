// Loading a shared library (dlopen / LoadLibrary).
#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace rsdkair {

class Module {
public:
    // nullptr on failure, with the reason in error.
    static std::unique_ptr<Module> open(const std::filesystem::path &path, std::string &error);

    ~Module();
    Module(const Module &) = delete;
    Module &operator=(const Module &) = delete;

    // nullptr if the module does not export name.
    void *symbol(const char *name) const;

    template <typename Fn> Fn function(const char *name) const { return reinterpret_cast<Fn>(symbol(name)); }

private:
    explicit Module(void *handle) : handle_(handle) {}
    void *handle_;
};

} // namespace rsdkair
