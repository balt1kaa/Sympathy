#include "common.h"
#include <compressapi.h>
#include <stdio.h>

#pragma comment(lib, "Cabinet.lib")

namespace {

const uint32_t kBlockSize = 8u * 1024 * 1024;

struct Writer {
    HANDLE h = INVALID_HANDLE_VALUE;

    uint64_t Tell() const
    {
        LARGE_INTEGER zero, pos;
        zero.QuadPart = 0;
        if (!SetFilePointerEx(h, zero, &pos, FILE_CURRENT)) return 0;
        return (uint64_t)pos.QuadPart;
    }

    bool Seek(uint64_t p) const
    {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)p;
        return SetFilePointerEx(h, li, NULL, FILE_BEGIN) != 0;
    }

    bool Write(const void* data, size_t n) const
    {
        const uint8_t* p = (const uint8_t*)data;
        while (n) {
            DWORD chunk = (DWORD)((n > 0x10000000u) ? 0x10000000u : n);
            DWORD put = 0;
            if (!WriteFile(h, p, chunk, &put, NULL) || put == 0) return false;
            p += put;
            n -= put;
        }
        return true;
    }

    template <class T> bool WritePod(const T& v) const { return Write(&v, sizeof(T)); }
};

std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0, NULL, NULL);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, NULL, NULL);
    return out;
}

struct Item {
    std::wstring name;
    uint64_t     size;
};

bool ListPayload(const std::wstring& dir, std::vector<Item>* out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Item it;
        it.name = fd.cFileName;
        it.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        out->push_back(it);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return !out->empty();
}

bool CopyRaw(const std::wstring& src, const std::wstring& dst)
{
    if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) return true;
    wprintf(L"cannot copy %s -> %s: %s\n", src.c_str(), dst.c_str(),
            LastErrorText(GetLastError()).c_str());
    return false;
}

}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 4) {
        wprintf(L"usage: pack.exe <payload-dir> <setup_raw.exe> <setup.exe>\n");
        return 2;
    }
    const std::wstring payloadDir = argv[1];
    const std::wstring rawExe     = argv[2];
    const std::wstring outExe     = argv[3];

    std::vector<Item> items;
    if (!ListPayload(payloadDir, &items)) {
        wprintf(L"no files in %s\n", payloadDir.c_str());
        return 1;
    }

    uint64_t totalRaw = 0;
    for (const Item& it : items) totalRaw += it.size;

    if (!CopyRaw(rawExe, outExe)) return 1;

    Writer w;
    w.h = CreateFileW(outExe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (w.h == INVALID_HANDLE_VALUE) {
        wprintf(L"cannot open %s: %s\n", outExe.c_str(), LastErrorText(GetLastError()).c_str());
        return 1;
    }

    LARGE_INTEGER endOfCode;
    if (!GetFileSizeEx(w.h, &endOfCode)) { CloseHandle(w.h); return 1; }
    const uint64_t archiveOffset = (uint64_t)endOfCode.QuadPart;
    w.Seek(archiveOffset);

    ArchiveHeader hdr;
    hdr.magic             = kArchiveMagic;
    hdr.version           = kFormatVersion;
    hdr.fileCount         = (uint32_t)items.size();
    hdr.blockSize         = kBlockSize;
    hdr.totalUncompressed = totalRaw;
    if (!w.WritePod(hdr)) { CloseHandle(w.h); return 1; }

    COMPRESSOR_HANDLE comp = NULL;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, NULL, &comp)) {
        wprintf(L"cannot start the compressor: %s\n", LastErrorText(GetLastError()).c_str());
        CloseHandle(w.h);
        return 1;
    }

    std::vector<uint8_t> raw(kBlockSize), out(kBlockSize + 4096);
    bool ok = true;

    for (const Item& it : items) {
        const std::wstring path = Join(payloadDir, it.name);
        HANDLE in = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (in == INVALID_HANDLE_VALUE) {
            wprintf(L"cannot read %s\n", path.c_str());
            ok = false; break;
        }

        const std::string nameUtf8 = WideToUtf8(it.name);
        const uint32_t nameBytes = (uint32_t)nameUtf8.size();
        const uint32_t blockCount =
            (uint32_t)((it.size + kBlockSize - 1) / kBlockSize);

        ok &= w.WritePod(nameBytes);
        ok &= w.Write(nameUtf8.data(), nameBytes);

        const uint64_t fixupPos = w.Tell();
        ok &= w.WritePod(it.size);
        ok &= w.WritePod((uint32_t)0);
        ok &= w.WritePod(blockCount);

        std::vector<BlockEntry> table((size_t)blockCount);
        memset(table.data(), 0, sizeof(BlockEntry) * blockCount);
        ok &= w.Write(table.data(), sizeof(BlockEntry) * blockCount);
        if (!ok) { CloseHandle(in); break; }

        uint32_t crc = 0;
        uint64_t left = it.size;
        for (uint32_t b = 0; b < blockCount && ok; ++b) {
            DWORD want = (DWORD)((left > kBlockSize) ? kBlockSize : left);
            DWORD got = 0;
            if (!ReadFile(in, raw.data(), want, &got, NULL) || got != want) {
                wprintf(L"short read on %s\n", path.c_str());
                ok = false; break;
            }
            left -= got;
            crc = Crc32(raw.data(), got, crc);

            SIZE_T produced = 0;
            bool stored = false;
            if (!Compress(comp, raw.data(), got, out.data(), out.size(), &produced) ||
                produced >= got) {
                stored = true;
                produced = got;
            }

            table[b].rawSize        = got;
            table[b].compressedSize = (uint32_t)produced;
            table[b].stored         = stored ? 1u : 0u;

            ok &= w.Write(stored ? raw.data() : out.data(), produced);
        }
        CloseHandle(in);
        if (!ok) break;

        const uint64_t afterData = w.Tell();
        ok &= w.Seek(fixupPos + sizeof(uint64_t));
        ok &= w.WritePod(crc);
        ok &= w.Seek(fixupPos + sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t));
        ok &= w.Write(table.data(), sizeof(BlockEntry) * blockCount);
        ok &= w.Seek(afterData);
        if (!ok) break;

        uint64_t packed = 0;
        for (const BlockEntry& be : table) packed += be.compressedSize;
        wprintf(L"  %-24s %10s -> %10s\n", it.name.c_str(),
                FormatBytes(it.size).c_str(), FormatBytes(packed).c_str());
    }

    CloseCompressor(comp);

    if (ok) {
        const uint64_t archiveEnd = w.Tell();

        Trailer tr;
        tr.archiveOffset = archiveOffset;
        tr.archiveSize   = archiveEnd - archiveOffset;
        tr.magic         = kTrailerMagic;
        tr.version       = kFormatVersion;
        ok &= w.WritePod(tr);

        if (ok) {
            wprintf(L"\n  payload   %s in %u files\n", FormatBytes(totalRaw).c_str(),
                    (unsigned)items.size());
            wprintf(L"  archive   %s\n", FormatBytes(tr.archiveSize).c_str());
            wprintf(L"  setup.exe %s\n", FormatBytes(archiveEnd + sizeof(Trailer)).c_str());
            wprintf(L"  ratio     %.1f%%\n", 100.0 * (double)tr.archiveSize / (double)totalRaw);
        }
    }

    CloseHandle(w.h);
    if (!ok) {
        DeleteFileW(outExe.c_str());
        wprintf(L"pack failed\n");
        return 1;
    }
    return 0;
}
