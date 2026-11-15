#include "logger.h"
#include "config.h"
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace Logger
{
    static std::string s_LogFilePath = "mods\\Framework_MOD\\mod.log";
    static FILE* s_LogFile = nullptr;
    static std::mutex s_LogMutex;
    static bool s_Initialized = false;

    static const char* LevelToString(Level level)
    {
        switch (level)
        {
            case Level::Debug: return "DEBUG";
            case Level::Info:  return "INFO ";
            case Level::Warn:  return "WARN ";
            case Level::Error: return "ERROR";
            default:           return "LOG  ";
        }
    }

    void Initialize(const std::string& outputDir)
    {
        std::lock_guard<std::mutex> lock(s_LogMutex);
        if (s_Initialized && s_LogFile)
            return;

        if (!outputDir.empty())
        {
            s_LogFilePath = outputDir + "\\mod.log";
        }
        else
        {
            s_LogFilePath = GetModDirectory() + "mod.log";
        }

        // Preserve previous session's log if it exists and has content
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(s_LogFilePath.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            FindClose(hFind);
            if (fd.nFileSizeLow > 0 || fd.nFileSizeHigh > 0)
            {
                std::string prevPath = s_LogFilePath + ".prev";
                CopyFileA(s_LogFilePath.c_str(), prevPath.c_str(), FALSE);
            }
        }

        s_LogFile = fopen(s_LogFilePath.c_str(), "w");
        if (s_LogFile)
        {
            fprintf(s_LogFile, "======================================================================\n");
            fprintf(s_LogFile, "  Crazy Taxi Mod Framework - Clean Diagnostic Log\n");
            fprintf(s_LogFile, "======================================================================\n\n");
            fflush(s_LogFile);
        }

        s_Initialized = true;
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(s_LogMutex);
        if (s_LogFile)
        {
            fprintf(s_LogFile, "\n[SESSION END] Log shutdown cleanly.\n");
            fflush(s_LogFile);
            fclose(s_LogFile);
            s_LogFile = nullptr;
        }
        s_Initialized = false;
    }

    void Log(Level level, const char* tag, const char* fmt, ...)
    {
        std::lock_guard<std::mutex> lock(s_LogMutex);
        if (!s_Initialized)
        {
            Initialize();
        }

        char msgBuffer[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(msgBuffer, sizeof(msgBuffer), fmt, args);
        va_end(args);

        SYSTEMTIME st;
        GetLocalTime(&st);

        char lineBuffer[1280];
        snprintf(lineBuffer, sizeof(lineBuffer), "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] [%s] %s\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            LevelToString(level),
            tag ? tag : "Core",
            msgBuffer);

        if (s_LogFile)
        {
            fputs(lineBuffer, s_LogFile);
            fflush(s_LogFile); // Immediate flush so logs survive crash
        }

        OutputDebugStringA(lineBuffer);
    }

    void Info(const char* tag, const char* fmt, ...)
    {
        char buffer[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        Log(Level::Info, tag, "%s", buffer);
    }

    void Warn(const char* tag, const char* fmt, ...)
    {
        char buffer[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        Log(Level::Warn, tag, "%s", buffer);
    }

    void Error(const char* tag, const char* fmt, ...)
    {
        char buffer[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        Log(Level::Error, tag, "%s", buffer);
    }

    void Debug(const char* tag, const char* fmt, ...)
    {
        char buffer[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        Log(Level::Debug, tag, "%s", buffer);
    }

    void LogCrash(EXCEPTION_POINTERS* pExceptionInfo)
    {
        if (!pExceptionInfo || !pExceptionInfo->ExceptionRecord)
            return;

        DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;
        uintptr_t addr = (uintptr_t)pExceptionInfo->ExceptionRecord->ExceptionAddress;

        char modName[MAX_PATH] = "Unknown";
        uintptr_t modBase = 0;
        HMODULE hMod = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)addr, &hMod) && hMod)
        {
            modBase = (uintptr_t)hMod;
            GetModuleFileNameA(hMod, modName, MAX_PATH);
            const char* slash = strrchr(modName, '\\');
            if (slash) memmove(modName, slash + 1, strlen(slash));
        }

        uintptr_t rva = (modBase != 0) ? (addr - modBase) : addr;

        char crashBuf[1024];
        snprintf(crashBuf, sizeof(crashBuf),
            "\n**********************************************************************\n"
            "CRASH DETECTED!\n"
            "Exception Code:   0x%08X\n"
            "Fault Address:    0x%p (%s + 0x%X)\n"
            "Registers (x86):\n"
            "  EAX: 0x%08X  EBX: 0x%08X  ECX: 0x%08X  EDX: 0x%08X\n"
            "  ESI: 0x%08X  EDI: 0x%08X  ESP: 0x%08X  EBP: 0x%08X\n"
            "  EIP: 0x%08X\n"
            "**********************************************************************\n",
            code, (void*)addr, modName, (unsigned int)rva,
            pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Eax : 0,
            pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Ebx : 0,
            pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Ecx : 0,
            pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Edx : 0,
            pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Esi : 0,
            pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Edi : 0,
            pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Esp : 0,
            pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Ebp : 0,
            pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Eip : 0
        );

        OutputDebugStringA(crashBuf);

        if (s_LogFile)
        {
            fputs(crashBuf, s_LogFile);
            fflush(s_LogFile);
        }

        FILE* fpCrash = fopen("mods\\Framework_MOD\\crash.log", "a");
        if (!fpCrash) fpCrash = fopen("crazytaxi_crash.log", "a");
        if (fpCrash)
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(fpCrash, "[%04d-%02d-%02d %02d:%02d:%02d]\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            fputs(crashBuf, fpCrash);
            fflush(fpCrash);
            fclose(fpCrash);
        }
    }

    LONG WINAPI CrashHandler(EXCEPTION_POINTERS* pExceptionInfo)
    {
        LogCrash(pExceptionInfo);
        return EXCEPTION_CONTINUE_SEARCH;
    }
}
