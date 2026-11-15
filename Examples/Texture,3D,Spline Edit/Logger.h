#pragma once
#include <windows.h>
#include <string>

enum LogLevel
{
    LOG_ERROR = 0,
    LOG_WARNING = 1,
    LOG_INFO = 2
};

class Logger
{
public:
    static void Init(const std::string& logFilePath, int level = 2);
    static void Log(LogLevel level, const char* fmt, ...);
    static void LogError(const char* fmt, ...);
    static void LogWarning(const char* fmt, ...);
    static void LogInfo(const char* fmt, ...);
    static void SetLogLevel(int level);
    static void Shutdown();

private:
    static std::string s_LogPath;
    static int s_LogLevel;
    static CRITICAL_SECTION s_CS;
    static bool s_Initialized;
    static FILE* s_LogFile;
};
