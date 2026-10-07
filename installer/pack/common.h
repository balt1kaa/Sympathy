#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>

static const uint32_t kTrailerMagic  = 0x53595054u;
static const uint32_t kArchiveMagic  = 0x53594152u;
static const uint32_t kFormatVersion = 1;

#pragma pack(push, 1)
struct Trailer {
    uint64_t archiveOffset;
    uint64_t archiveSize;
    uint32_t magic;
    uint32_t version;
};

struct ArchiveHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t fileCount;
    uint32_t blockSize;
    uint64_t totalUncompressed;
};

struct BlockEntry {
    uint32_t compressedSize;
    uint32_t rawSize;
    uint32_t stored;
};
#pragma pack(pop)

static const wchar_t* const kMediaCoreRelative = L"Adobe\\Common\\Plug-ins\\7.0\\MediaCore";
static const wchar_t* const kInstallFolder     = L"sympathy";
static const wchar_t* const kVendorFolder      = L"ashen one";
static const wchar_t* const kUninstallExe      = L"uninstall.exe";
static const wchar_t* const kRegUninstallKey   =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\sympathy";

static const wchar_t* const kProductVersion = L"1.0.0";
static const wchar_t* const kPublisher      = L"ashen one";


static const wchar_t* const kLegacyFolders[] = { L"AV1_Import", L"ashen one", L"harpies" };

static const wchar_t* const kAdobeProcesses[] = {
    L"Adobe Premiere Pro.exe",
    L"AfterFX.exe",
    L"Adobe Media Encoder.exe",
};

static const wchar_t* const kIndent = L"\t";

struct Reporter {
    virtual void Step(const std::wstring& text) = 0;
    virtual void Bytes(uint64_t done, uint64_t total) = 0;
    virtual ~Reporter() {}
};

std::wstring  ExePath();
std::wstring  DirOf(const std::wstring& path);
std::wstring  Join(const std::wstring& dir, const std::wstring& leaf);
bool          FileExists(const std::wstring& path);
bool          DirExists(const std::wstring& path);
bool          EnsureDirTree(const std::wstring& path);
bool          RemoveTreeEx(const std::wstring& path, DWORD timeoutMs);
bool          RemoveTree(const std::wstring& path);
std::wstring  ProgramFilesDir();
std::wstring  TempDir();

std::wstring  RoamingDirShown();

std::wstring  SettingsDirShown();
std::wstring  LastErrorText(DWORD err);
std::wstring  FormatBytes(uint64_t n);
uint32_t      Crc32(const void* data, size_t len, uint32_t seed);

std::vector<std::wstring> RunningAdobeApps();

bool ExtractPayload(const std::wstring& destDir, Reporter& rep,
                    const std::wstring& skipName = std::wstring());

bool ListPayload(std::vector<std::wstring>* names);

bool HasPayload(uint64_t* uncompressedTotal);

bool WriteUninstallerCopy(const std::wstring& destPath, std::wstring* err);

struct EditionInfo {
    std::wstring prmPath;
    std::wstring prmName;
    std::wstring displayName;
    bool         embedded;
    std::wstring embeddedName;
};

bool FindEdition(EditionInfo* out, std::wstring* problem);

std::wstring MediaCoreDir();
std::wstring TargetDir();

std::wstring SupportDir();

bool DoInstall(Reporter& rep);

std::wstring InstalledPluginDir();

bool DoUninstall(Reporter& rep);

bool DoUninstallCleanup(const std::wstring& installDir, DWORD parentPid, Reporter& rep);

enum class UiMode { Install, Uninstall };

int RunUi(HINSTANCE hInst, UiMode mode, const std::wstring& cleanupDir);
