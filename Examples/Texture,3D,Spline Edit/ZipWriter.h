#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include <cstdint>
#include <algorithm>

// ---------------------------------------------------------------------------------
// Standard Self-Contained PKWARE ZIP Archive Writer (No external tools or Python needed)
// ---------------------------------------------------------------------------------
class ZipWriter
{
#pragma pack(push, 1)
    struct LocalHeader
    {
        uint32_t signature = 0x04034b50;
        uint16_t versionNeeded = 20;
        uint16_t flags = 0;
        uint16_t compressionMethod = 0; // 0 = STORE (PNGs are already deflated)
        uint16_t lastModTime = 0;
        uint16_t lastModDate = 0;
        uint32_t crc32 = 0;
        uint32_t compressedSize = 0;
        uint32_t uncompressedSize = 0;
        uint16_t filenameLength = 0;
        uint16_t extraFieldLength = 0;
    };

    struct CentralHeader
    {
        uint32_t signature = 0x02014b50;
        uint16_t versionMadeBy = 20;
        uint16_t versionNeeded = 20;
        uint16_t flags = 0;
        uint16_t compressionMethod = 0;
        uint16_t lastModTime = 0;
        uint16_t lastModDate = 0;
        uint32_t crc32 = 0;
        uint32_t compressedSize = 0;
        uint32_t uncompressedSize = 0;
        uint16_t filenameLength = 0;
        uint16_t extraFieldLength = 0;
        uint16_t commentLength = 0;
        uint16_t diskNumberStart = 0;
        uint16_t internalAttributes = 0;
        uint32_t externalAttributes = 0x81B60020;
        uint32_t relativeOffset = 0;
    };

    struct EndOfCentralDir
    {
        uint32_t signature = 0x06054b50;
        uint16_t diskNumber = 0;
        uint16_t startDisk = 0;
        uint16_t totalEntriesDisk = 0;
        uint16_t totalEntries = 0;
        uint32_t sizeOfCentralDir = 0;
        uint32_t offsetOfCentralDir = 0;
        uint16_t commentLength = 0;
    };
#pragma pack(pop)

    struct Entry
    {
        std::string relPath;
        uint32_t crc32 = 0;
        uint32_t size = 0;
        uint32_t offset = 0;
        uint16_t dosTime = 0;
        uint16_t dosDate = 0;
    };

    static uint32_t CalculateCRC32(const uint8_t* data, size_t length)
    {
        static uint32_t table[256];
        static bool haveTable = false;
        if (!haveTable)
        {
            for (uint32_t i = 0; i < 256; i++)
            {
                uint32_t rem = i;
                for (int j = 0; j < 8; j++)
                {
                    if (rem & 1)
                        rem = (rem >> 1) ^ 0xEDB88320;
                    else
                        rem >>= 1;
                }
                table[i] = rem;
            }
            haveTable = true;
        }

        uint32_t crc = ~0u;
        for (size_t i = 0; i < length; i++)
        {
            uint8_t octet = data[i];
            crc = (crc >> 8) ^ table[(crc & 0xFF) ^ octet];
        }
        return ~crc;
    }

    static void GetDosDateTime(const FILETIME& ft, uint16_t& dosDate, uint16_t& dosTime)
    {
        FILETIME localFt;
        FileTimeToLocalFileTime(&ft, &localFt);
        SYSTEMTIME st;
        FileTimeToSystemTime(&localFt, &st);
        dosTime = (uint16_t)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond >> 1));
        dosDate = (uint16_t)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
    }

public:
    static bool CreateZipFromFolder(const std::string& sourceFolder, const std::string& zipFilePath, int* outCount = nullptr)
    {
        // 1. Collect all files recursively
        struct FileItem {
            std::string fullPath;
            std::string relPath;
            FILETIME lastWrite;
            uint32_t fileSize;
        };

        std::vector<FileItem> filesToZip;
        auto Enumerate = [&](auto& self, const std::string& currentDir, const std::string& relPrefix) -> void
        {
            std::string mask = currentDir + "\\*.*";
            WIN32_FIND_DATAA fd;
            HANDLE hFind = FindFirstFileA(mask.c_str(), &fd);
            if (hFind == INVALID_HANDLE_VALUE) return;
            do
            {
                if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
                    continue;

                // Skip temporary stamp backups or hidden folders
                if (fd.cFileName[0] == '.')
                    continue;

                std::string fullPath = currentDir + "\\" + fd.cFileName;
                std::string relPath = relPrefix.empty() ? fd.cFileName : (relPrefix + "/" + fd.cFileName);

                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    self(self, fullPath, relPath);
                }
                else
                {
                    // Convert backslashes to forward slashes for standard ZIP
                    std::replace(relPath.begin(), relPath.end(), '\\', '/');
                    filesToZip.push_back({ fullPath, relPath, fd.ftLastWriteTime, fd.nFileSizeLow });
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        };

        Enumerate(Enumerate, sourceFolder, "");

        if (filesToZip.empty())
            return false;

        // Ensure parent directory of zip exists
        size_t lastSlash = zipFilePath.find_last_of("\\/");
        if (lastSlash != std::string::npos)
        {
            std::string pDir = zipFilePath.substr(0, lastSlash);
            CreateDirectoryA(pDir.c_str(), NULL);
        }

        std::ofstream zipFile(zipFilePath, std::ios::binary | std::ios::trunc);
        if (!zipFile.is_open())
            return false;

        std::vector<Entry> entries;
        entries.reserve(filesToZip.size());

        // 2. Write local file headers + file data
        for (const auto& f : filesToZip)
        {
            std::ifstream in(f.fullPath, std::ios::binary);
            if (!in.is_open()) continue;

            std::vector<uint8_t> buffer(f.fileSize);
            if (f.fileSize > 0)
            {
                in.read(reinterpret_cast<char*>(buffer.data()), f.fileSize);
            }
            in.close();

            uint32_t crc = CalculateCRC32(buffer.data(), buffer.size());
            uint16_t dosDate = 0, dosTime = 0;
            GetDosDateTime(f.lastWrite, dosDate, dosTime);

            uint32_t localOffset = static_cast<uint32_t>(zipFile.tellp());

            LocalHeader lh;
            lh.compressionMethod = 0; // STORE
            lh.lastModTime = dosTime;
            lh.lastModDate = dosDate;
            lh.crc32 = crc;
            lh.compressedSize = static_cast<uint32_t>(buffer.size());
            lh.uncompressedSize = static_cast<uint32_t>(buffer.size());
            lh.filenameLength = static_cast<uint16_t>(f.relPath.size());
            lh.extraFieldLength = 0;

            zipFile.write(reinterpret_cast<const char*>(&lh), sizeof(lh));
            zipFile.write(f.relPath.data(), f.relPath.size());
            if (!buffer.empty())
            {
                zipFile.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
            }

            Entry ent;
            ent.relPath = f.relPath;
            ent.crc32 = crc;
            ent.size = static_cast<uint32_t>(buffer.size());
            ent.offset = localOffset;
            ent.dosDate = dosDate;
            ent.dosTime = dosTime;
            entries.push_back(ent);
        }

        // 3. Write Central Directory
        uint32_t centralDirOffset = static_cast<uint32_t>(zipFile.tellp());
        for (const auto& ent : entries)
        {
            CentralHeader ch;
            ch.compressionMethod = 0;
            ch.lastModTime = ent.dosTime;
            ch.lastModDate = ent.dosDate;
            ch.crc32 = ent.crc32;
            ch.compressedSize = ent.size;
            ch.uncompressedSize = ent.size;
            ch.filenameLength = static_cast<uint16_t>(ent.relPath.size());
            ch.extraFieldLength = 0;
            ch.commentLength = 0;
            ch.relativeOffset = ent.offset;

            zipFile.write(reinterpret_cast<const char*>(&ch), sizeof(ch));
            zipFile.write(ent.relPath.data(), ent.relPath.size());
        }

        uint32_t centralDirEnd = static_cast<uint32_t>(zipFile.tellp());
        uint32_t centralDirSize = centralDirEnd - centralDirOffset;

        // 4. Write End of Central Directory
        EndOfCentralDir eocd;
        eocd.totalEntriesDisk = static_cast<uint16_t>(entries.size());
        eocd.totalEntries = static_cast<uint16_t>(entries.size());
        eocd.sizeOfCentralDir = centralDirSize;
        eocd.offsetOfCentralDir = centralDirOffset;

        zipFile.write(reinterpret_cast<const char*>(&eocd), sizeof(eocd));
        zipFile.close();

        if (outCount)
            *outCount = static_cast<int>(entries.size());

        return true;
    }
};

