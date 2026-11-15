#pragma once
#include <windows.h>
#include <string>

namespace Logger
{
    enum class Level
    {
        Debug,
        Info,
        Warn,
        Error
    };

    // Initializes logging to the specified directory (defaults to "mods/Framework_MOD/mod.log")
    void Initialize(const std::string& outputDir = "");

    // Flushes and closes log file
    void Shutdown();

    // Leveled logging methods with printf-style formatting
    void Log(Level level, const char* tag, const char* fmt, ...);

    void Info(const char* tag, const char* fmt, ...);
    void Warn(const char* tag, const char* fmt, ...);
    void Error(const char* tag, const char* fmt, ...);
    void Debug(const char* tag, const char* fmt, ...);

    // Crash logging
    void LogCrash(EXCEPTION_POINTERS* pExceptionInfo);
    LONG WINAPI CrashHandler(EXCEPTION_POINTERS* pExceptionInfo);
}
