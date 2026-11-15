#include "Logger.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

std::string Logger::s_LogPath = "";
int Logger::s_LogLevel = 2;
CRITICAL_SECTION Logger::s_CS;
bool Logger::s_Initialized = false;
FILE* Logger::s_LogFile = nullptr;

void Logger::Init(const std::string& logFilePath, int level)
{
    s_LogPath = logFilePath;
    s_LogLevel = level;

    if (!s_Initialized)
    {
        InitializeCriticalSection(&s_CS);
        s_Initialized = true;
    }

    EnterCriticalSection(&s_CS);
    if (s_LogFile)
    {
        fclose(s_LogFile);
        s_LogFile = nullptr;
    }

    s_LogFile = fopen(s_LogPath.c_str(), "a");
    if (s_LogFile)
    {
        time_t rawtime;
        time(&rawtime);
        struct tm timeinfo;
        localtime_s(&timeinfo, &rawtime);
        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);

        fprintf(s_LogFile, "\n=======================================================\n");
        fprintf(s_LogFile, "[%s] LyrasTextureMod Runtime Logger Initialized (Level %d)\n", timeStr, s_LogLevel);
        fprintf(s_LogFile, "=======================================================\n");
        fflush(s_LogFile);
    }
    LeaveCriticalSection(&s_CS);
}

void Logger::Shutdown()
{
    if (s_Initialized)
    {
        EnterCriticalSection(&s_CS);
        if (s_LogFile)
        {
            fclose(s_LogFile);
            s_LogFile = nullptr;
        }
        LeaveCriticalSection(&s_CS);
        DeleteCriticalSection(&s_CS);
        s_Initialized = false;
    }
}

void Logger::SetLogLevel(int level)
{
    s_LogLevel = level;
}

void Logger::Log(LogLevel level, const char* fmt, ...)
{
    if (level > s_LogLevel || s_LogPath.empty())
        return;

    va_list args;
    va_start(args, fmt);
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    const char* prefix = "[INFO] ";
    if (level == LOG_ERROR) prefix = "[ERROR] ";
    else if (level == LOG_WARNING) prefix = "[WARN] ";

    time_t rawtime;
    time(&rawtime);
    struct tm timeinfo;
    localtime_s(&timeinfo, &rawtime);
    char timeStr[32];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);

    if (s_Initialized)
        EnterCriticalSection(&s_CS);

    if (s_LogFile)
    {
        fprintf(s_LogFile, "[%s] %s%s\n", timeStr, prefix, buffer);
        fflush(s_LogFile);
    }
    else
    {
        FILE* fp = fopen(s_LogPath.c_str(), "a");
        if (fp)
        {
            fprintf(fp, "[%s] %s%s\n", timeStr, prefix, buffer);
            fclose(fp);
        }
    }

    OutputDebugStringA(("[LyrasTextureMod] " + std::string(prefix) + buffer + "\n").c_str());

    if (s_Initialized)
        LeaveCriticalSection(&s_CS);
}

void Logger::LogError(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Log(LOG_ERROR, "%s", buf);
}

void Logger::LogWarning(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Log(LOG_WARNING, "%s", buf);
}

void Logger::LogInfo(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Log(LOG_INFO, "%s", buf);
}
