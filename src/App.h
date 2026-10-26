#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

#include "Platform.h"

// Single-game native frontend. Owns the window, the embedded ROM and the
// emulated console; the core calls back into it through Platform.cpp.
class App
{
public:
    static std::string ExeDirectory();
    static void LogV(int level, const char* fmt, va_list args);

    int Run(int argc, char** argv);

    void OnEmuStop(melonDS::Platform::StopReason reason);
    void OnSaveWrite(const uint8_t* data, uint32_t len);

private:
    struct Impl;
    Impl* impl = nullptr;
};
