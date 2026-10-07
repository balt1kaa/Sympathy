#include "common.h"
#include <shlobj.h>
#include <tlhelp32.h>
#include <stdio.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

std::wstring ExePath()
{
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = GetModuleFileNameW(NULL, buf.data(), (DWORD)buf.size());
        if (n == 0) return std::wstring();
        if (n < buf.size() - 1) return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);
    }
}

std::wstring DirOf(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    return path.substr(0, slash);
}

std::wstring Join(const std::wstring& dir, const std::wstring& leaf)
{
    if (dir.empty()) return leaf;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + leaf;
    return dir + L"\\" + leaf;
}

bool FileExists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDirTree(const std::wstring& path)
{
    if (path.empty() || DirExists(path)) return DirExists(path);

    std::wstring parent = DirOf(path);
    if (!parent.empty() && parent.size() > 2 && !DirExists(parent)) {
        if (!EnsureDirTree(parent)) return false;
    }
    if (CreateDirectoryW(path.c_str(), NULL)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring ProgramFilesDir()
{
    PWSTR raw = NULL;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, NULL, &raw)))
        return L"C:\\Program Files";
    std::wstring out(raw);
    CoTaskMemFree(raw);
    return out;
}

std::wstring SettingsDirShown()
{
    return Join(RoamingDirShown(), kVendorFolder);
}

std::wstring RoamingDirShown()
{
    PWSTR raw = NULL;
    std::wstring roaming;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &raw))) {
        roaming.assign(raw);
        CoTaskMemFree(raw);
    }
    if (roaming.empty()) roaming = L"C:\\Users\\name\\AppData\\Roaming";

    const size_t appData = roaming.rfind(L"\\AppData\\");
    if (appData != std::wstring::npos && appData > 0) {
        const size_t account = roaming.rfind(L'\\', appData - 1);
        if (account != std::wstring::npos)
            roaming = roaming.substr(0, account + 1) + L"name" + roaming.substr(appData);
    }

    return roaming;
}

std::wstring TempDir()
{
    wchar_t buf[MAX_PATH + 1] = {0};
    DWORD n = GetTempPathW(MAX_PATH, buf);
    if (n == 0 || n > MAX_PATH) return L".";
    std::wstring out(buf, n);
    while (!out.empty() && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

bool RemoveTree(const std::wstring& path)
{
    return RemoveTreeEx(path, 4000);
}

bool RemoveTreeEx(const std::wstring& path, DWORD timeoutMs)
{
    if (!DirExists(path)) return true;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(path, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool ok = true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        std::wstring child = Join(path, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
            SetFileAttributesW(child.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            if (!RemoveTreeEx(child, timeoutMs)) ok = false;
        } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            if (!RemoveDirectoryW(child.c_str())) ok = false;
        } else {
            if (!DeleteFileW(child.c_str())) ok = false;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    const DWORD start = GetTickCount();
    for (;;) {
        if (RemoveDirectoryW(path.c_str())) return ok;

        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return ok;
        if (GetTickCount() - start >= timeoutMs) return false;
        Sleep(100);
    }
}

std::wstring LastErrorText(DWORD err)
{
    LPWSTR msg = NULL;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&msg, 0, NULL);

    std::wstring out;
    if (n && msg) {
        out.assign(msg, n);
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' '))
            out.pop_back();
    }
    if (msg) LocalFree(msg);

    wchar_t tail[32];
    swprintf(tail, 32, L" (0x%08X)", err);
    return out.empty() ? std::wstring(L"error") + tail : out + tail;
}

std::wstring FormatBytes(uint64_t n)
{
    wchar_t buf[64];
    if (n >= 1024ull * 1024 * 1024)
        swprintf(buf, 64, L"%.2f GB", (double)n / (1024.0 * 1024 * 1024));
    else if (n >= 1024ull * 1024)
        swprintf(buf, 64, L"%.1f MB", (double)n / (1024.0 * 1024));
    else if (n >= 1024)
        swprintf(buf, 64, L"%.1f KB", (double)n / 1024.0);
    else
        swprintf(buf, 64, L"%llu bytes", (unsigned long long)n);
    return buf;
}

uint32_t Crc32(const void* data, size_t len, uint32_t seed)
{
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }

    uint32_t c = seed ^ 0xFFFFFFFFu;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; ++i)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::vector<std::wstring> RunningAdobeApps()
{
    std::vector<std::wstring> found;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return found;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            for (const wchar_t* name : kAdobeProcesses) {
                if (_wcsicmp(pe.szExeFile, name) != 0) continue;

                bool already = false;
                for (const std::wstring& s : found)
                    if (s == name) { already = true; break; }
                if (!already) found.push_back(name);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}
