#include "sympathy.h"
#include "async_import.h"

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libswscale/swscale.h>
    #include <libswresample/swresample.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/samplefmt.h>
    #include <libavutil/channel_layout.h>
    #include <libavutil/opt.h>
    #include <libavutil/hwcontext.h>
}
#include <stdarg.h>
#include <stdlib.h>
#include <limits.h>
#include <direct.h>
#include <share.h>
#include <algorithm>
#include <map>
#include <string>
#include <mutex>
#include <vector>
#include <list>
#include <thread>
#include <condition_variable>

#include "CFHDDecoder.h"

static void Ashen_LoadFFmpegFromPluginDir(void)
{
    static bool done = false;
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    if (done) return;
    done = true;

    HMODULE self = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&Ashen_LoadFFmpegFromPluginDir, &self) || !self)
        return;

    wchar_t dir[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(self, dir, (DWORD)(sizeof(dir) / sizeof(dir[0])));
    if (n == 0 || n >= sizeof(dir) / sizeof(dir[0])) return;
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    *slash = 0;

    const wchar_t* stem[5] = { L"avutil", L"swresample", L"swscale", L"avcodec", L"avformat" };
    const int      major[5] = { LIBAVUTIL_VERSION_MAJOR, LIBSWRESAMPLE_VERSION_MAJOR,
                                LIBSWSCALE_VERSION_MAJOR, LIBAVCODEC_VERSION_MAJOR,
                                LIBAVFORMAT_VERSION_MAJOR };
    for (int i = 0; i < 5; ++i) {
        wchar_t full[MAX_PATH * 2];
        _snwprintf_s(full, sizeof(full) / sizeof(full[0]), _TRUNCATE,
                     L"%s\\%s-%d.dll", dir, stem[i], major[i]);
        LoadLibraryExW(full, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
}

static void Ashen_CheckSiblingPlugins(char* out, int outSize)
{
    out[0] = 0;

    HMODULE self = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&Ashen_CheckSiblingPlugins, &self) || !self)
        return;

    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(self, path, (DWORD)(sizeof(path) / sizeof(path[0])));
    if (n == 0 || n >= sizeof(path) / sizeof(path[0])) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    const wchar_t* selfName = slash + 1;
    *slash = 0;

    wchar_t glob[MAX_PATH * 2];
    _snwprintf_s(glob, sizeof(glob) / sizeof(glob[0]), _TRUNCATE, L"%s\\*.prm", path);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    int    others = 0;
    char   list[512]; list[0] = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (_wcsicmp(fd.cFileName, selfName) == 0) continue;

        char name8[256] = {0};
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name8, sizeof(name8) - 1, NULL, NULL);
        if (others++) strncat_s(list, sizeof(list), ", ", _TRUNCATE);
        strncat_s(list, sizeof(list), name8, _TRUNCATE);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (others > 0)
        _snprintf_s(out, outSize, _TRUNCATE, "%s", list);
}

#define Ashen_FLIP_VERTICAL 1

static const PrTime kAshenTicksPerSecond = 254016000000LL;

static const char* Ashen_LogPath(int prev)
{
    static char cur[1024], old[1024];
    static int  resolved = 0;
    if (!resolved) {
        resolved = 1;
        char appdata[512] = {0}; size_t n = 0;
        if (getenv_s(&n, appdata, sizeof(appdata), "APPDATA") == 0 && n > 0) {
            char dir[768];
            _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\Ashen One", appdata);
            _mkdir(dir);
            _snprintf_s(cur, sizeof(cur), _TRUNCATE, "%s\\Sympathy.log",      dir);
            _snprintf_s(old, sizeof(old), _TRUNCATE, "%s\\sympathy.prev.log", dir);
        } else {
            char tmp[512] = {0}; size_t m = 0;
            if (getenv_s(&m, tmp, sizeof(tmp), "TEMP") != 0 || m == 0)
                strncpy_s(tmp, sizeof(tmp), ".", _TRUNCATE);
            _snprintf_s(cur, sizeof(cur), _TRUNCATE, "%s\\Sympathy.log",      tmp);
            _snprintf_s(old, sizeof(old), _TRUNCATE, "%s\\sympathy.prev.log", tmp);
        }
    }
    return prev ? old : cur;
}
#define kAshenLogPath     Ashen_LogPath(0)
#define kAshenLogPrevPath Ashen_LogPath(1)

static std::mutex g_logMutex;
static double ashen_now_ms();
static FILE* g_logFile   = NULL;
static double g_logT0    = 0.0;
static int    g_logPending = 0;
static int   g_logLevel  = -1;
static int   g_logOpened = 0;

#define ASHEN_MAX_SESSIONS_DEFAULT 2
struct AshenConfig {
    int  loaded;
    int  disable;
    int  disableMultithreading;
    int  logLevel;
    int  cacheFrames;
    int  yuv422;
    int  force8bit;
    int  fullChroma;
    int  hwSessions;
    int  hwaccel;
    char overrideExtensions[512];
    char path[1024];
};
static AshenConfig g_cfg;
static std::mutex  g_cfgMutex;

static const AshenConfig& Ashen_Cfg()
{
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    if (g_cfg.loaded) return g_cfg;
    g_cfg.loaded = 1;
    g_cfg.disable = 0; g_cfg.disableMultithreading = 0;
    g_cfg.logLevel = 0; g_cfg.cacheFrames = 0; g_cfg.yuv422 = 1; g_cfg.force8bit = 0; g_cfg.fullChroma = 1; g_cfg.hwSessions = ASHEN_MAX_SESSIONS_DEFAULT;
    g_cfg.hwaccel = 0;
    g_cfg.overrideExtensions[0] = 0;
    g_cfg.path[0] = 0;
    return g_cfg;
}

static int Ashen_EnvInt(const char* name, int* out)
{
    char buf[32] = {0}; size_t n = 0;
    if (getenv_s(&n, buf, sizeof(buf), name) == 0 && n > 0) { *out = atoi(buf); return 1; }
    return 0;
}

static int Ashen_CacheFrames();

static int Ashen_LogLevel()
{
    if (g_logLevel < 0) {
        int v = Ashen_Cfg().logLevel;
        Ashen_EnvInt("ASHEN_LOG_LEVEL", &v);
        g_logLevel = (v < 0) ? 0 : v;
    }
    return g_logLevel;
}

static void ashenlog(const char* fmt, ...)
{
    if (Ashen_LogLevel() <= 0) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_logOpened) {
        g_logOpened = 1;
        remove(kAshenLogPrevPath);
        rename(kAshenLogPath, kAshenLogPrevPath);
        g_logFile = _fsopen(kAshenLogPath, "w", _SH_DENYNO);
        if (g_logFile) {
            g_logT0 = ashen_now_ms();
            fprintf(g_logFile, "[0.0] RUN build=%s %s level=%d avcodec=%d.%d license=%s\n",
                    __DATE__, __TIME__,
                    g_logLevel, LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR,
                    avutil_license());
            {
                const AshenConfig& c = Ashen_Cfg();
                fprintf(g_logFile, "[0.0] CFG file=%s disable=%d mt_off=%d cacheFrames=%d "
                                   "yuv422=%d overrideExtensions=\"%s\"\n",
                        c.path[0] ? c.path : "(none)", c.disable, c.disableMultithreading,
                        Ashen_CacheFrames(), c.yuv422, c.overrideExtensions);
            }
            {
                char siblings[512];
                Ashen_CheckSiblingPlugins(siblings, sizeof(siblings));
                if (siblings[0])
                    fprintf(g_logFile,
                            "[0.0] CONFLICT another importer is installed beside this one: %s"
                            "  -- Premiere loads both and they claim the same extensions."
                            "  Remove all but one and restart.\n", siblings);
            }
            fflush(g_logFile);
        }
    }
    if (!g_logFile) return;
    fprintf(g_logFile, "[%.1f] ", ashen_now_ms() - g_logT0);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fputc('\n', g_logFile);
    if (++g_logPending >= 64) { g_logPending = 0; fflush(g_logFile); }
}

#define ashenlog2(...) do { if (Ashen_LogLevel() >= 2) ashenlog(__VA_ARGS__); } while (0)
#define ashenlog3(...) do { if (Ashen_LogLevel() >= 3) ashenlog(__VA_ARGS__); } while (0)

static const char* Ashen_ClipName(const char* path)
{
    if (!path) return "?";
    const char* a = strrchr(path, '\\');
    const char* b = strrchr(path, '/');
    const char* s = (a > b) ? a : b;
    return s ? s + 1 : path;
}

struct ClipStat {
    char  clip[160];
    char  codec[40];
    char  decoder[40];
    int   hw, intra;
    std::vector<float> durFwd, durSeek, durFc, wait;
    int   fail, getReady, getWait, getSync, getCache;
    ClipStat() { clip[0]=codec[0]=decoder[0]=0; hw=intra=0; fail=getReady=getWait=getSync=getCache=0; }
};
static std::map<int, ClipStat*> g_stats;
static std::mutex               g_statMutex;
static const size_t kAshenStatCap = 200000;

static ClipStat* Ashen_Stat(int importerID)
{
    auto it = g_stats.find(importerID);
    if (it != g_stats.end()) return it->second;
    ClipStat* c = new ClipStat();
    g_stats[importerID] = c;
    return c;
}

static void Ashen_StatSample(int importerID, const char* bucket, double ms)
{
    if (Ashen_LogLevel() <= 0) return;
    std::lock_guard<std::mutex> g(g_statMutex);
    ClipStat* c = Ashen_Stat(importerID);
    std::vector<float>* v = NULL;
    switch (bucket[0]) {
        case 'f': v = (bucket[1] == 'c') ? &c->durFc : &c->durFwd; break;
        case 's': v = &c->durSeek; break;
        case 'w': v = &c->wait;    break;
        default:  return;
    }
    if (v->size() < kAshenStatCap) v->push_back((float)ms);
}

static void Ashen_StatFail(int importerID)
{
    if (Ashen_LogLevel() <= 0) return;
    std::lock_guard<std::mutex> g(g_statMutex);
    Ashen_Stat(importerID)->fail++;
}

static void Ashen_StatGet(int importerID, const char* kind)
{
    if (Ashen_LogLevel() <= 0) return;
    std::lock_guard<std::mutex> g(g_statMutex);
    ClipStat* c = Ashen_Stat(importerID);
    if      (!strcmp(kind, "ready")) c->getReady++;
    else if (!strcmp(kind, "wait"))  c->getWait++;
    else if (!strcmp(kind, "sync"))  c->getSync++;
    else if (!strcmp(kind, "cache")) c->getCache++;
}

static float Ashen_Pct(std::vector<float>& v, double p)
{
    if (v.empty()) return 0.f;
    size_t k = (size_t)(p * (v.size() - 1) + 0.5);
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

static void Ashen_SumBucket(const char* clip, const char* codec, const char* name,
                            std::vector<float>& v)
{
    if (v.empty()) return;
    double sum = 0; float mx = 0;
    for (float f : v) { sum += f; if (f > mx) mx = f; }
    float p50 = Ashen_Pct(v, 0.50), p95 = Ashen_Pct(v, 0.95);
    ashenlog("SUM clip=%s codec=%s bucket=%s n=%d avg=%.1f p50=%.1f p95=%.1f max=%.1f",
             clip, codec, name, (int)v.size(), sum / v.size(), p50, p95, mx);
}

static void Ashen_LogCacheStats(const char* clip, const char* codec);

static void Ashen_StatDump(int importerID)
{
    ClipStat* c = NULL;
    {
        std::lock_guard<std::mutex> g(g_statMutex);
        auto it = g_stats.find(importerID);
        if (it == g_stats.end()) return;
        c = it->second; g_stats.erase(it);
    }
    const char* clip  = c->clip[0]  ? c->clip  : "?";
    const char* codec = c->codec[0] ? c->codec : "?";
    ashenlog("SUM clip=%s codec=%s decoder=%s hw=%d intra=%d frames=%d fail=%d "
             "get_ready=%d get_wait=%d get_sync=%d get_cache=%d",
             clip, codec, c->decoder[0] ? c->decoder : "?", c->hw, c->intra,
             (int)(c->durFwd.size() + c->durSeek.size() + c->durFc.size()), c->fail,
             c->getReady, c->getWait, c->getSync, c->getCache);
    Ashen_SumBucket(clip, codec, "fwd",  c->durFwd);
    Ashen_SumBucket(clip, codec, "seek", c->durSeek);
    Ashen_SumBucket(clip, codec, "fc",   c->durFc);
    Ashen_SumBucket(clip, codec, "wait", c->wait);
    Ashen_LogCacheStats(clip, codec);
    delete c;
}

static double ashen_now_ms();
static enum AVPixelFormat Ashen_AvFormatFor(PrPixelFormat f);

void AshenGetLog(int importerID, const char* kind, int frame, double waitMs, int queue,
                 int intent, int width, int height, int pixfmt)
{
    if (waitMs >= 0.0)
        ashenlog2("GET kind=%s imp=%d f=%d wait=%.1f q=%d intent=%d size=%dx%d fmt=%d",
                  kind, importerID, frame, waitMs, queue, intent, width, height, pixfmt);
    else
        ashenlog2("GET kind=%s imp=%d f=%d q=%d intent=%d size=%dx%d fmt=%d",
                  kind, importerID, frame, queue, intent, width, height, pixfmt);
    Ashen_StatGet(importerID, kind);
    if (waitMs > 0.0) Ashen_StatSample(importerID, "wait", waitMs);
}

double AshenNowMs(void) { return ashen_now_ms(); }

void AshenFlushLog(int importerID, int dropped, int inProgress, double ms)
{
    ashenlog2("FLUSH imp=%d dropped=%d inprog=%d dur=%.1f",
              importerID, dropped, inProgress, ms);
}

void AshenUndeliverableFormatLog(int wanted, int preferred, int nOffered)
{
    char w[5] = { (char)(wanted & 0xff), (char)((wanted >> 8) & 0xff),
                  (char)((wanted >> 16) & 0xff), (char)((wanted >> 24) & 0xff), 0 };
    for (int k = 0; k < 4; ++k) if (w[k] < 32 || w[k] > 126) w[k] = '?';
    ashenlog("FMT-UNDELIVERABLE host_wanted=%d '%s' our_preferred=%d offered=%d "
             "-> substituting bgra 8u (NOT a format the host asked for)",
             wanted, w, preferred, nOffered);
}

void AshenDupFrameLog(int importerID, int frame, int waiters)
{
    ashenlog("DUPFRAME imp=%d f=%d waiters=%d -> second reader decodes its own copy",
             importerID, frame, waiters);
}

int Ashen_CanDeliver(PrPixelFormat f)
{
    if (f == PrPixelFormat_BGRA_4444_16u && Ashen_Cfg().force8bit) return 0;
    return (Ashen_AvFormatFor(f) != AV_PIX_FMT_NONE) ? 1 : 0;
}

void AshenHostFormatsLog(int count, const char* list)
{
    ashenlog("HOSTFMT count=%d offered=[%s]", count, list ? list : "");
}

void AshenBufferFailLog(int importerID, int frame, int w, int h, const char* stage)
{
    ashenlog("BUFFAIL imp=%d f=%d %s size=%dx%d bytes=%lld",
             importerID, frame, stage, w, h,
             (long long)w * (long long)h * 4ll);
}

void AshenCacheAddLog(int importerID, int frame, int rc)
{
    if (rc != 0)
        ashenlog("HOSTCACHE imp=%d f=%d add_failed rc=%d", importerID, frame, rc);
}

static double ashen_now_ms()
{
    LARGE_INTEGER freq, ctr;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    return 1000.0 * (double)ctr.QuadPart / (double)freq.QuadPart;
}
static int g_decodeCalls = 0;

static int Ashen_CodecValidated(enum AVCodecID id)
{
    switch (id) {
        case AV_CODEC_ID_AV1:     return 1;
        case AV_CODEC_ID_HQX:     return 1;
        case AV_CODEC_ID_HQ_HQA:  return 1;
        case AV_CODEC_ID_CLLC:    return 1;
        case AV_CODEC_ID_CFHD:    return 1;
        case AV_CODEC_ID_RAWVIDEO:return 1;
        case AV_CODEC_ID_FFV1:    return 1;
        case AV_CODEC_ID_JPEG2000:return 1;
        case AV_CODEC_ID_APV:     return 1;
        case AV_CODEC_ID_VP9:     return 1;
        case AV_CODEC_ID_VP8:     return 1;
        case AV_CODEC_ID_V210:    return 1;
        case AV_CODEC_ID_V410:    return 1;
        case AV_CODEC_ID_R210:    return 1;
        case AV_CODEC_ID_R10K:    return 1;
        case AV_CODEC_ID_UTVIDEO: return 1;
        case AV_CODEC_ID_FFVHUFF: return 1;
        case AV_CODEC_ID_HUFFYUV: return 1;
        case AV_CODEC_ID_MAGICYUV:return 1;
        default: return 0;
    }
}

static int Ashen_MxfIsOpAtom(AVFormatContext* fmt)
{
    if (!fmt) return 0;
    const AVDictionaryEntry* op = av_dict_get(fmt->metadata, "operational_pattern_ul", NULL, 0);
    return (op && op->value && strstr(op->value, "0d010201.10")) ? 1 : 0;
}

static int Ashen_CodecBanned(enum AVCodecID id, const char* fmtName,
                             AVFormatContext* fmt, AVStream* st, const char* path)
{
    if (!fmtName) fmtName = "";
    char ext[16] = {0};
    if (path) {
        const char* dot = strrchr(path, '.');
        if (dot && dot[1]) {
            size_t n = 0;
            for (const char* c = dot + 1; *c && n < sizeof(ext) - 1; ++c, ++n)
                ext[n] = (char)tolower((unsigned char)*c);
        }
    }
    const int isMov   = !strcmp(ext, "mov");
    const int isMp4   = !strcmp(ext, "mp4") || !strcmp(ext, "m4v");
    const int isAvi   = !strcmp(ext, "avi");
    const int isMxf   = !strcmp(ext, "mxf");
    const int isoBmff = isMov || isMp4;
    const int profile = st ? st->codecpar->profile : AV_PROFILE_UNKNOWN;
    const int opAtom  = isMxf && Ashen_MxfIsOpAtom(fmt);
    const int op1a    = isMxf && !opAtom;

    if (id == AV_CODEC_ID_VVC && isoBmff) return 1;

    if (id == AV_CODEC_ID_DNXHD) return 1;

    if (isoBmff && (id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_HEVC)) return 1;

    if (isMp4 && id == AV_CODEC_ID_MPEG2VIDEO) return 1;

    if (isMov) {
        switch (id) {
            case AV_CODEC_ID_PRORES:
            case AV_CODEC_ID_DNXHD:
            case AV_CODEC_ID_MPEG4:
            case AV_CODEC_ID_V210:
            case AV_CODEC_ID_R210:
            case AV_CODEC_ID_CFHD:
                return 1;
            case AV_CODEC_ID_RAWVIDEO:
                if (st && (st->codecpar->format == AV_PIX_FMT_RGB24 ||
                           st->codecpar->format == AV_PIX_FMT_UYVY422)) return 1;
                break;
            default: break;
        }
    }

    if (isoBmff || isAvi) {
        switch (id) {
            case AV_CODEC_ID_MJPEG:    return 1;
            case AV_CODEC_ID_DVVIDEO:  return 1;
            default: break;
        }
    }
    if (strstr(fmtName, "mov") != NULL) {
        switch (id) {
            case AV_CODEC_ID_QTRLE:    return 1;
            case AV_CODEC_ID_PNG:      return 1;
            default: break;
        }
    }

    if (isMov || isAvi) {
        switch (id) {
            case AV_CODEC_ID_MJPEG:    return 1;
            case AV_CODEC_ID_DVVIDEO:  return 1;
            default: break;
        }
    }
    if (isMov) {
        switch (id) {
            case AV_CODEC_ID_QTRLE:    return 1;
            case AV_CODEC_ID_PNG:      return 1;
            default: break;
        }
    }

    if (isMov && id == AV_CODEC_ID_R10K) return 1;

    if (isAvi && id == AV_CODEC_ID_V210) return 1;

    if (opAtom) {
        switch (id) {
            case AV_CODEC_ID_MPEG2VIDEO:
            case AV_CODEC_ID_JPEG2000:
            case AV_CODEC_ID_DNXHD:
                return 1;
            default: break;
        }
    }

    if (op1a) {
        switch (id) {
            case AV_CODEC_ID_MPEG2VIDEO:
            case AV_CODEC_ID_JPEG2000:
            case AV_CODEC_ID_DNXHD:
            case AV_CODEC_ID_PRORES:
                return 1;
            case AV_CODEC_ID_H264:
                if (profile == AV_PROFILE_H264_HIGH_422_INTRA ||
                    profile == AV_PROFILE_H264_HIGH_10_INTRA) return 1;
                break;
            default: break;
        }
    }

    return 0;
}

static const char* Ashen_CuvidName(enum AVCodecID id)
{
    switch (id) {
        case AV_CODEC_ID_AV1:  return "av1_cuvid";
        case AV_CODEC_ID_VP9:  return "vp9_cuvid";
        case AV_CODEC_ID_VP8:  return "vp8_cuvid";
        case AV_CODEC_ID_HEVC: return "hevc_cuvid";
        case AV_CODEC_ID_H264: return "h264_cuvid";
        default: return NULL;
    }
}

static const char* Ashen_QsvName(enum AVCodecID id)
{
    switch (id) {
        case AV_CODEC_ID_AV1:  return "av1_qsv";
        case AV_CODEC_ID_VP9:  return "vp9_qsv";
        case AV_CODEC_ID_VP8:  return "vp8_qsv";
        case AV_CODEC_ID_HEVC: return "hevc_qsv";
        case AV_CODEC_ID_H264: return "h264_qsv";
        default: return NULL;
    }
}

static const char* Ashen_AmfName(enum AVCodecID id)
{
    switch (id) {
        case AV_CODEC_ID_AV1:  return "av1_amf";
        case AV_CODEC_ID_VP9:  return "vp9_amf";
        case AV_CODEC_ID_HEVC: return "hevc_amf";
        case AV_CODEC_ID_H264: return "h264_amf";
        default: return NULL;
    }
}

static int Ashen_IsIntraCodec(enum AVCodecID id)
{
    switch (id) {
        case AV_CODEC_ID_CFHD:
        case AV_CODEC_ID_HQX:
        case AV_CODEC_ID_HQ_HQA:
        case AV_CODEC_ID_CLLC:
        case AV_CODEC_ID_PRORES:
        case AV_CODEC_ID_DNXHD:
        case AV_CODEC_ID_RAWVIDEO:
        case AV_CODEC_ID_V210:
        case AV_CODEC_ID_V410:
        case AV_CODEC_ID_R210:
        case AV_CODEC_ID_MJPEG:
        case AV_CODEC_ID_JPEG2000:
        case AV_CODEC_ID_APV:
        case AV_CODEC_ID_FFV1:
        case AV_CODEC_ID_UTVIDEO:
        case AV_CODEC_ID_FFVHUFF:
        case AV_CODEC_ID_HUFFYUV:
        case AV_CODEC_ID_MAGICYUV:
            return 1;
        default:
            return 0;
    }
}

static int Ashen_DecoderProducesFrame(AVFormatContext* fmt, int vs, enum AVCodecID id)
{
    const AVCodec* dec = avcodec_find_decoder(id);
    if (!dec || !fmt || vs < 0) return 0;
    AVCodecContext* c = avcodec_alloc_context3(dec);
    if (!c) return 0;
    avcodec_parameters_to_context(c, fmt->streams[vs]->codecpar);
    c->codec_id     = id;
    c->thread_count = 1;
    int ok = 0;
    if (avcodec_open2(c, dec, NULL) == 0) {
        AVPacket* pk = av_packet_alloc();
        AVFrame*  fr = av_frame_alloc();
        int fed = 0;
        while (pk && fr && !ok && fed < 16 && av_read_frame(fmt, pk) >= 0) {
            if (pk->stream_index == vs) {
                ++fed;
                if (avcodec_send_packet(c, pk) == 0 && avcodec_receive_frame(c, fr) == 0)
                    ok = 1;
            }
            av_packet_unref(pk);
        }
        if (pk) av_packet_free(&pk);
        if (fr) av_frame_free(&fr);
    }
    avcodec_free_context(&c);
    av_seek_frame(fmt, vs, 0, AVSEEK_FLAG_BACKWARD);
    return ok;
}

static const enum AVCodecID kAshenCodecCandidates[] = {
    AV_CODEC_ID_HEVC, AV_CODEC_ID_H264, AV_CODEC_ID_MPEG2VIDEO, AV_CODEC_ID_DNXHD,
};

static enum AVCodecID Ashen_ResolveCodecId(AVFormatContext* fmt, AVStream* st, int vs)
{
    if (!fmt || !st) return AV_CODEC_ID_NONE;
    if (st->codecpar->codec_id != AV_CODEC_ID_NONE) return st->codecpar->codec_id;
    for (size_t i = 0; i < sizeof(kAshenCodecCandidates) / sizeof(kAshenCodecCandidates[0]); ++i)
        if (Ashen_DecoderProducesFrame(fmt, vs, kAshenCodecCandidates[i]))
            return kAshenCodecCandidates[i];
    return AV_CODEC_ID_NONE;
}

static int Ashen_IsUncompressed(enum AVCodecID id)
{
    switch (id) {
        case AV_CODEC_ID_RAWVIDEO:
        case AV_CODEC_ID_V210:
        case AV_CODEC_ID_V410:
        case AV_CODEC_ID_R210:
            return 1;
        default:
            return 0;
    }
}

static int Ashen_LowresHint(int intent, double playbackRatio)
{
    if (intent == imRenderIntent_Scrubbing) return 2;
    if (playbackRatio > 0.0 && playbackRatio < 0.95) return 2;
    return 0;
}

static int Ashen_ColorspaceCoeff(int colorspace)
{
    switch (colorspace) {
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:   return SWS_CS_ITU601;
        case AVCOL_SPC_SMPTE240M:   return SWS_CS_SMPTE240M;
        case AVCOL_SPC_FCC:         return SWS_CS_FCC;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:   return SWS_CS_BT2020;
        case AVCOL_SPC_BT709:
        default:                    return SWS_CS_ITU709;
    }
}

static int Ashen_IsHdrTransfer(int trc)
{
    return (trc == AVCOL_TRC_SMPTE2084 || trc == AVCOL_TRC_ARIB_STD_B67) ? 1 : 0;
}

static PrPixelFormat Ashen_NativePixelFormat(enum AVPixelFormat pf, int colorspace, int trc)
{
    int is601 = (Ashen_ColorspaceCoeff(colorspace) == SWS_CS_ITU601);
    const int hdr = Ashen_IsHdrTransfer(trc);

    {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(pf);
        if (d && d->nb_components >= 3 && d->log2_chroma_w == 1 && d->log2_chroma_h == 0) {
            int on = Ashen_Cfg().yuv422;
            Ashen_EnvInt("ASHEN_YUV422", &on);
            if (on && !hdr) return is601 ? PrPixelFormat_UYVY_422_8u_601
                                         : PrPixelFormat_UYVY_422_8u_709;
        }
    }

    {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(pf);
        if (d && d->comp[0].depth > 8 && !Ashen_Cfg().force8bit)
            return PrPixelFormat_BGRA_4444_16u;
    }

    switch (pf) {
        case AV_PIX_FMT_UYVY422:
            return is601 ? PrPixelFormat_UYVY_422_8u_601 : PrPixelFormat_UYVY_422_8u_709;
        case AV_PIX_FMT_YUYV422:
            return is601 ? PrPixelFormat_YUYV_422_8u_601 : PrPixelFormat_YUYV_422_8u_709;
        default:
            break;
    }

    return PrPixelFormat_Invalid;
}

static int Ashen_CineFormCanEmit(PrPixelFormat f)
{
    switch (f) {
        case PrPixelFormat_UYVY_422_8u_601: case PrPixelFormat_UYVY_422_8u_709:
        case PrPixelFormat_YUYV_422_8u_601: case PrPixelFormat_YUYV_422_8u_709:
        case PrPixelFormat_BGRA_4444_8u:
        case PrPixelFormat_BGRA_4444_16u:
            return 1;
        default:
            return 0;
    }
}

static enum AVPixelFormat Ashen_AvFormatFor(PrPixelFormat f)
{
    switch (f) {
        case PrPixelFormat_UYVY_422_8u_601: case PrPixelFormat_UYVY_422_8u_709:
            return AV_PIX_FMT_UYVY422;
        case PrPixelFormat_YUYV_422_8u_601: case PrPixelFormat_YUYV_422_8u_709:
            return AV_PIX_FMT_YUYV422;
        case PrPixelFormat_BGRA_4444_8u:
            return AV_PIX_FMT_BGRA;
        case PrPixelFormat_BGRA_4444_16u:
            return AV_PIX_FMT_BGRA64LE;
        default:
            return AV_PIX_FMT_NONE;
    }
}

static int Ashen_NativeRowBytes(PrPixelFormat f, int width)
{
    switch (f) {
        case PrPixelFormat_UYVY_422_8u_601: case PrPixelFormat_UYVY_422_8u_709:
        case PrPixelFormat_YUYV_422_8u_601: case PrPixelFormat_YUYV_422_8u_709:
            return width * 2;
        case PrPixelFormat_BGRA_4444_16u:
            return width * 8;
        default:
            return width * 4;
    }
}

static void Ashen_Utf16ToUtf8(const prUTF16Char* in, char* out, int outSize)
{
    WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<LPCWSTR>(in), -1,
                        out, outSize, NULL, NULL);
}

static AVRational Ashen_FrameRate(AVFormatContext* fmt, AVStream* st)
{
    AVRational fr = av_guess_frame_rate(fmt, st, NULL);
    if (fr.num <= 0 || fr.den <= 0) { fr.num = 25; fr.den = 1; return fr; }
    if ((kAshenTicksPerSecond * (PrTime)fr.den) % (PrTime)fr.num == 0) return fr;

    static const AVRational kStd[] = {
        {24000,1001},{24,1},{25,1},{30000,1001},{30,1},{48000,1001},{48,1},{50,1},
        {60000,1001},{60,1},{100,1},{120000,1001},{120,1},
    };
    const double want = (double)fr.num / (double)fr.den;
    AVRational best = fr; double bestErr = 0.005;
    for (size_t i = 0; i < sizeof(kStd)/sizeof(kStd[0]); ++i) {
        double v = (double)kStd[i].num / (double)kStd[i].den;
        double e = (v > want) ? (v - want) / want : (want - v) / want;
        if (e < bestErr) { bestErr = e; best = kStd[i]; }
    }
    return best;
}

static prMALError Ashen_Probe(const prUTF16Char* path16, AshenClip* f)
{
    double _t0 = ashen_now_ms();
    char path[2048];
    Ashen_Utf16ToUtf8(path16, path, sizeof(path));

    AVFormatContext* fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
        ashenlog("PROBE verdict=unreadable clip=%s", Ashen_ClipName(path));
        return imBadFile;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        ashenlog("PROBE verdict=unparsable clip=%s", Ashen_ClipName(path));
        return imBadFile;
    }
    int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vs < 0) {
        avformat_close_input(&fmt);
        ashenlog("PROBE verdict=decline clip=%s reason=no_video_stream", Ashen_ClipName(path));
        return imBadFile;
    }

    AVStream* st = fmt->streams[vs];

    const char* fmtName = fmt->iformat ? fmt->iformat->name : "";
    int guessed = (st->codecpar->codec_id == AV_CODEC_ID_NONE);
    enum AVCodecID cid = Ashen_ResolveCodecId(fmt, st, vs);

    int canDecode = (cid != AV_CODEC_ID_NONE) && avcodec_find_decoder(cid) != NULL;
    int banned    = Ashen_CodecBanned(cid, fmtName, fmt, st, path);
    int take      = canDecode && !banned;

    if (guessed)
        ashenlog("PROBE note=essence_unnamed clip=%s -> %s", Ashen_ClipName(path),
                 canDecode ? avcodec_get_name(cid) : "no candidate decoded it");

    const char* why = guessed                   ? "guessed"
                    : Ashen_CodecValidated(cid) ? "validated"
                    : "unvalidated";

    if (Ashen_Cfg().disable) {
        ashenlog("PROBE verdict=decline clip=%s reason=config_disable", Ashen_ClipName(path));
        avformat_close_input(&fmt);
        return imBadFile;
    }
    if (take && Ashen_Cfg().overrideExtensions[0]) {
        const char* dot = strrchr(path, '.');
        int allowed = 0;
        if (dot && dot[1]) {
            char want[32] = {0};
            strncpy_s(want, sizeof(want), dot + 1, _TRUNCATE);
            char list[512];
            strncpy_s(list, sizeof(list), Ashen_Cfg().overrideExtensions, _TRUNCATE);
            char* ctx = NULL;
            for (char* t = strtok_s(list, ", \t;", &ctx); t; t = strtok_s(NULL, ", \t;", &ctx)) {
                if (*t == '.') ++t;
                if (!_stricmp(t, want)) { allowed = 1; break; }
            }
        }
        if (!allowed) {
            ashenlog("PROBE verdict=decline clip=%s reason=overrideExtensions", Ashen_ClipName(path));
            avformat_close_input(&fmt);
            return imBadFile;
        }
    }
    if (!take) {
        ashenlog("PROBE verdict=decline clip=%s codec=%s container=%s",
               Ashen_ClipName(path), avcodec_get_name(cid), fmtName);
        avformat_close_input(&fmt);
        return imBadFile;
    }
    ashenlog("PROBE verdict=take clip=%s codec=%s container=%s via=%s",
           Ashen_ClipName(path), avcodec_get_name(cid), fmtName, why);

    AVRational fr = Ashen_FrameRate(fmt, st);
    {
        AVRational raw = av_guess_frame_rate(fmt, st, NULL);
        if (raw.num != fr.num || raw.den != fr.den)
            ashenlog("PROBE note=frame_rate_snapped clip=%s %d/%d -> %d/%d",
                     Ashen_ClipName(path), raw.num, raw.den, fr.num, fr.den);
    }
    if (fr.num <= 0 || fr.den <= 0) {
        ashenlog("PROBE note=no_frame_rate clip=%s -> assuming 25fps", Ashen_ClipName(path));
        fr.num = 25; fr.den = 1;
    }
    double fps = (double)fr.num / fr.den;
    double durSec = (fmt->duration > 0) ? fmt->duration / (double)AV_TIME_BASE : 0.0;

    int64_t nbFrames = st->nb_frames;
    if (nbFrames <= 0)
        nbFrames = (int64_t)(durSec * fps + 0.5);
    if (nbFrames <= 0)
        nbFrames = 1;

    memset(f, 0, sizeof(*f));
    f->hasVideo       = kPrTrue;
    f->hasAudio       = kPrFalse;
    f->videoSubtype   = 'RAW ';
    {
        PrPixelFormat native = Ashen_NativePixelFormat(
            (enum AVPixelFormat)st->codecpar->format, st->codecpar->color_space,
            st->codecpar->color_trc);
        f->pixelFormat = (native != PrPixelFormat_Invalid) ? native
                                                           : PrPixelFormat_BGRA_4444_8u;
    }
    f->depth          = (f->pixelFormat == PrPixelFormat_BGRA_4444_16u)   ? 64 :
                        (f->pixelFormat == PrPixelFormat_UYVY_422_8u_601 ||
                         f->pixelFormat == PrPixelFormat_UYVY_422_8u_709 ||
                         f->pixelFormat == PrPixelFormat_YUYV_422_8u_601 ||
                         f->pixelFormat == PrPixelFormat_YUYV_422_8u_709) ? 16 : 32;
    f->width          = st->codecpar->width;
    f->height         = st->codecpar->height;
    f->numFrames      = (csSDK_uint32)nbFrames;
    f->frameRate      = (PrTime)(kAshenTicksPerSecond * fr.den / (fr.num ? fr.num : 25));
    f->pixelAspectNum = 1;
    f->pixelAspectDen = 1;
    f->fieldType      = 0;
    {
        f->colorPrimaries = (csSDK_int32)st->codecpar->color_primaries;
        f->colorTrc       = (csSDK_int32)st->codecpar->color_trc;
        f->colorMatrix    = (csSDK_int32)st->codecpar->color_space;
        const AVPixFmtDescriptor* bd = av_pix_fmt_desc_get((enum AVPixelFormat)st->codecpar->format);
        f->mediaBitDepth  = (bd && bd->comp[0].depth > 0) ? bd->comp[0].depth : 8;
        if (Ashen_IsHdrTransfer(f->colorTrc))
            ashenlog("PROBE hdr clip=%s transfer=%s primaries=%d matrix=%d bits=%d",
                     Ashen_ClipName(path),
                     (f->colorTrc == AVCOL_TRC_SMPTE2084) ? "PQ" : "HLG",
                     f->colorPrimaries, f->colorMatrix, f->mediaBitDepth);
    }
    {
        const AVPixFmtDescriptor* ad = av_pix_fmt_desc_get((enum AVPixelFormat)st->codecpar->format);
        int srcAlpha = (ad && (ad->flags & AV_PIX_FMT_FLAG_ALPHA)) ? 1 : 0;
        int fmtCarriesAlpha = (f->pixelFormat == PrPixelFormat_BGRA_4444_8u ||
                               f->pixelFormat == PrPixelFormat_BGRA_4444_16u);
        f->hasAlpha = (srcAlpha && fmtCarriesAlpha) ? 1 : 0;
        if (srcAlpha && !fmtCarriesAlpha)
            ashenlog("PROBE note=alpha_dropped clip=%s reason=delivery_format_has_no_alpha",
                     Ashen_ClipName(path));
    }

    {
        int totalCh = 0, nStreams = 0, rate = 0;
        double adur = 0.0;
        const char* firstCodec = "";
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            AVStream* ast = fmt->streams[i];
            if (ast->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
            int ch = ast->codecpar->ch_layout.nb_channels;
            if (ch <= 0) continue;
            if (rate == 0) { rate = ast->codecpar->sample_rate;
                             firstCodec = avcodec_get_name(ast->codecpar->codec_id); }
            else if (ast->codecpar->sample_rate != rate) continue;
            if (totalCh >= 8) break;
            totalCh += ch; ++nStreams;
            double d = (ast->duration > 0) ? ast->duration * av_q2d(ast->time_base) : durSec;
            if (d > adur) adur = d;
        }
        if (totalCh > 0) {
            f->hasAudio   = kPrTrue;
            f->sampleRate = rate;
            if (totalCh <= 1)      f->channelType = kPrAudioChannelType_Mono;
            else if (totalCh >= 6) f->channelType = kPrAudioChannelType_51;
            else                   f->channelType = kPrAudioChannelType_Stereo;
            int presented = (totalCh <= 1) ? 1 : (totalCh >= 6 ? 6 : 2);
            if (adur <= 0.0) adur = durSec;
            f->numSampleFrames = (PrAudioSample)(adur * f->sampleRate);
            ashenlog("PROBE audio clip=%s codec=%s streams=%d channels=%d presented=%d "
                   "rate=%.0f samples=%lld%s",
                   Ashen_ClipName(path), firstCodec, nStreams, totalCh, presented,
                   (double)f->sampleRate, (long long)f->numSampleFrames,
                   (presented < totalCh) ? " CLAMPED" : "");
        }
    }

    ashenlog("PROBE info clip=%s codec=%s size=%dx%d fps=%.3f frames=%lld dur=%.1fms",
           Ashen_ClipName(path), avcodec_get_name(st->codecpar->codec_id),
           f->width, f->height, fps, (long long)nbFrames, ashen_now_ms() - _t0);

    avformat_close_input(&fmt);
    return malNoError;
}

struct AshenSession {
    int              importerID;
    char             clip[160];
    char             codec[40];
    bool             busy;
    int              doomed;
    char             path[2048];
    AVFormatContext* fmt;
    struct DirectIO* dio;
    AVCodecContext*  ctx;
    int              vstream;
    int              usingHw;
    int              codecId;
    int              decFails;
    int              intraOnly;
    int              lowres;
    int              maxLowres;
    CFHD_DecoderRef  cfhd;
    int              useCineForm;
    int              cfhdPrepared;
    int              cfhdW, cfhdH;
    int              cfhdOutW, cfhdOutH;
    int              cfhdFmt;
    struct SwsContext* sws;
    int              swsW, swsH, swsSrcFmt, swsDstFmt;
    int              swsSrcW, swsSrcH;
    int64_t          lastPts;
    int              lastFrame;
    double           cvtMs;
    int              poolSize;
    int              fwdFrames;
    int              atEof;
    AVPacket*        pkt;
    AVFrame*         frame;
    AVFrame*         keep;
    AVBufferRef*        hwDev;
    enum AVPixelFormat  hwPixFmt;
    AVFrame*            hwSw;
};

static enum AVPixelFormat Ashen_GetFormat(AVCodecContext* ctx, const enum AVPixelFormat* fmts)
{
    AshenSession* s = (AshenSession*)ctx->opaque;
    if (s && s->hwPixFmt != AV_PIX_FMT_NONE)
        for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == s->hwPixFmt) return *p;
    return fmts[0];
}

static AVFrame* Ashen_ToSystemMemory(AshenSession* s, AVFrame* f)
{
    if (!f || s->hwPixFmt == AV_PIX_FMT_NONE || f->format != (int)s->hwPixFmt) return f;
    if (!s->hwSw) return f;
    av_frame_unref(s->hwSw);
    if (av_hwframe_transfer_data(s->hwSw, f, 0) < 0) return NULL;
    av_frame_copy_props(s->hwSw, f);
    return s->hwSw;
}

static int Ashen_NextFrame(struct AshenSession* s);

static std::map<int, AshenSession*> g_sessions;
static std::mutex                 g_sessionMutex;

static void FrameCache_Release(int importerID);

struct AshenFileFacts {
    int intraOnly;
    int ready;
    int computing;
};
static std::map<std::string, AshenFileFacts> g_facts;
static std::mutex                            g_factsMutex;
static std::condition_variable               g_factsCv;

static int Ashen_FactsAcquire(const char* path, int* intraOnly)
{
    std::unique_lock<std::mutex> lk(g_factsMutex);
    for (;;) {
        AshenFileFacts& f = g_facts[path];
        if (f.ready)     { *intraOnly = f.intraOnly; return 1; }
        if (f.computing) { g_factsCv.wait(lk); continue; }
        f.computing = 1;
        return 0;
    }
}

static void Ashen_FactsPublish(const char* path, int intraOnly)
{
    {
        std::lock_guard<std::mutex> lk(g_factsMutex);
        AshenFileFacts& f = g_facts[path];
        f.intraOnly = intraOnly; f.ready = 1; f.computing = 0;
    }
    g_factsCv.notify_all();
}

static const int ASHEN_MAX_SESSIONS = 8;
static std::map<int, std::vector<AshenSession*>> g_pool;
static std::map<int, int>                        g_poolCreating;
static std::map<int, unsigned long long>         g_poolTicket;
static std::map<int, unsigned long long>         g_poolServe;
static std::mutex                                g_poolMutex;
static std::condition_variable                   g_poolCv;

struct DirectIO {
    HANDLE   h;
    int64_t  size;
    int64_t  pos;
    DWORD    sector;
    uint8_t* abuf;
    DWORD    abufCap;
};

static int DirectIO_Read(void* opaque, uint8_t* buf, int want)
{
    DirectIO* d = (DirectIO*)opaque;
    if (want <= 0) return 0;
    if (d->pos >= d->size) return AVERROR_EOF;
    int64_t end = d->pos + want; if (end > d->size) end = d->size;
    DWORD wantN = (DWORD)(end - d->pos);
    if (wantN == 0) return AVERROR_EOF;
    LARGE_INTEGER li; li.QuadPart = d->pos;
    if (!SetFilePointerEx(d->h, li, NULL, FILE_BEGIN)) return AVERROR(EIO);
    DWORD got = 0;
    if (!ReadFile(d->h, buf, wantN, &got, NULL)) return AVERROR(EIO);
    if (got == 0) return AVERROR_EOF;
    d->pos += got;
    return (int)got;
}

static int64_t DirectIO_Seek(void* opaque, int64_t off, int whence)
{
    DirectIO* d = (DirectIO*)opaque;
    if (whence & AVSEEK_SIZE) return d->size;
    whence &= ~AVSEEK_FORCE;
    if (whence == SEEK_CUR)      off += d->pos;
    else if (whence == SEEK_END) off += d->size;
    if (off < 0) off = 0;
    d->pos = off;
    return d->pos;
}

static AVFormatContext* Ashen_OpenDirect(const char* path, DirectIO** outDio)
{
    wchar_t wpath[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 2048) == 0) return NULL;
    HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return NULL; }

    const DWORD sector  = 4096;
    const int   avioCap = 4 << 20;
    DirectIO* d = new DirectIO();
    d->h = h; d->size = sz.QuadPart; d->pos = 0; d->sector = sector;
    d->abufCap = 0;
    d->abuf = NULL;
    unsigned char* iob = (unsigned char*)av_malloc(avioCap);
    AVIOContext* avio = iob ? avio_alloc_context(iob, avioCap, 0, d, DirectIO_Read, NULL, DirectIO_Seek) : NULL;
    AVFormatContext* fmt = avio ? avformat_alloc_context() : NULL;
    if (!fmt) {
        if (avio) avio_context_free(&avio);
        if (iob)  av_free(iob);
        if (d->abuf) VirtualFree(d->abuf, 0, MEM_RELEASE);
        CloseHandle(h); delete d; return NULL;
    }
    fmt->pb = avio;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0) {
        av_freep(&avio->buffer); avio_context_free(&avio);
        if (d->abuf) VirtualFree(d->abuf, 0, MEM_RELEASE);
        CloseHandle(h); delete d; return NULL;
    }
    *outDio = d;
    return fmt;
}

static void Ashen_CloseDirect(AVFormatContext* fmt, DirectIO* d)
{
    if (fmt) {
        AVIOContext* avio = fmt->pb;
        avformat_close_input(&fmt);
        if (avio) { av_freep(&avio->buffer); avio_context_free(&avio); }
    }
    if (d) {
        if (d->abuf) VirtualFree(d->abuf, 0, MEM_RELEASE);
        if (d->h && d->h != INVALID_HANDLE_VALUE) CloseHandle(d->h);
        delete d;
    }
}

static void Ashen_CloseSession(AshenSession* s)
{
    if (!s) return;
    if (s->cfhd)  CFHD_CloseDecoder(s->cfhd);
    if (s->sws)   sws_freeContext(s->sws);
    if (s->hwSw)  av_frame_free(&s->hwSw);
    if (s->hwDev) av_buffer_unref(&s->hwDev);
    if (s->keep)  av_frame_free(&s->keep);
    if (s->frame) av_frame_free(&s->frame);
    if (s->pkt)   av_packet_free(&s->pkt);
    if (s->ctx)   avcodec_free_context(&s->ctx);
    if (s->dio)        { Ashen_CloseDirect(s->fmt, s->dio); s->fmt = NULL; s->dio = NULL; }
    else if (s->fmt)   avformat_close_input(&s->fmt);
    delete s;
}

static void Ashen_RetirePool(std::vector<AshenSession*>& vec, int* closedOut, int* doomedOut)
{
    int closed = 0, doomed = 0;
    for (AshenSession* s : vec) {
        if (s->busy) { s->doomed = 1; ++doomed; }
        else         { Ashen_CloseSession(s); ++closed; }
    }
    vec.clear();
    if (closedOut) *closedOut = closed;
    if (doomedOut) *doomedOut = doomed;
}

static void Ashen_ReleaseImporter(int importerID)
{
    {
        std::lock_guard<std::mutex> lock(g_poolMutex);
        auto it = g_pool.find(importerID);
        if (it != g_pool.end()) {
            int closed = 0, doomed = 0;
            Ashen_RetirePool(it->second, &closed, &doomed);
            ashenlog("CLOSE imp=%d closed=%d busy_doomed=%d", importerID, closed, doomed);
        }
    }
    Ashen_StatDump(importerID);
    { std::lock_guard<std::mutex> lk(g_logMutex); if (g_logFile) fflush(g_logFile); }
}

static void Ashen_QuietImporter(int importerID)
{
    int closed = 0, kept = 0;
    {
        std::lock_guard<std::mutex> lock(g_poolMutex);
        auto it = g_pool.find(importerID);
        if (it == g_pool.end()) return;
        std::vector<AshenSession*>& vec = it->second;
        for (size_t i = 0; i < vec.size(); ) {
            if (vec[i]->busy) { ++kept; ++i; continue; }
            Ashen_CloseSession(vec[i]);
            vec.erase(vec.begin() + i);
            ++closed;
        }
    }
    if (closed || kept)
        ashenlog("QUIET imp=%d closed=%d busy_kept=%d", importerID, closed, kept);
}

static AshenSession* Ashen_CreateSession(int importerID, const char* path)
{
    double  _tOpen = ashen_now_ms(), openMs = 0.0, infoMs = 0.0, probeMs = 0.0;
    int     scanCached = 0, scanPackets = 0;
    int64_t scanBytes = 0;

    AshenSession* s = new AshenSession();
    memset(s, 0, sizeof(*s));
    s->hwPixFmt = AV_PIX_FMT_NONE;
    s->importerID = importerID;
    strncpy_s(s->path, sizeof(s->path), path, _TRUNCATE);
    s->lastPts = INT64_MIN;
    s->lastFrame = INT_MIN;

    s->fmt = Ashen_OpenDirect(path, &s->dio);
    if (!s->fmt) {
        if (avformat_open_input(&s->fmt, path, NULL, NULL) < 0) { Ashen_CloseSession(s); return NULL; }
    }
    openMs = ashen_now_ms() - _tOpen;
    { double _ti = ashen_now_ms();
      if (avformat_find_stream_info(s->fmt, NULL) < 0) { Ashen_CloseSession(s); return NULL; }
      infoMs = ashen_now_ms() - _ti; }
    s->vstream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (s->vstream < 0) { Ashen_CloseSession(s); return NULL; }

    AVStream* st = s->fmt->streams[s->vstream];

    const AVCodec* dec = NULL;
    int usingHw = 0;
    const int hwMode = Ashen_Cfg().hwaccel;
    const int mxfForced = (st->codecpar->codec_id == AV_CODEC_ID_NONE);
    const enum AVCodecID cid_ = Ashen_ResolveCodecId(s->fmt, st, s->vstream);
    const char* hwName = NULL;

    const char* cand[3] = { NULL, NULL, NULL };
    int nCand = 0;
    if      (hwMode == 3) { cand[nCand++] = Ashen_QsvName(cid_); }
    else if (hwMode == 4) { cand[nCand++] = Ashen_AmfName(cid_); }
    else if (hwMode == 0) {
        cand[nCand++] = Ashen_CuvidName(cid_);
        cand[nCand++] = Ashen_AmfName(cid_);
        cand[nCand++] = Ashen_QsvName(cid_);
    }

    const AVCodec* swDec = avcodec_find_decoder(cid_);
    for (int i = 0; i < nCand && !usingHw; ++i) {
        if (!cand[i]) continue;
        const AVCodec* c = avcodec_find_decoder_by_name(cand[i]);
        if (!c) continue;
        AVCodecContext* probe = avcodec_alloc_context3(c);
        if (!probe) continue;
        avcodec_parameters_to_context(probe, st->codecpar);
        if (mxfForced) probe->codec_id = cid_;
        probe->thread_count = Ashen_Cfg().disableMultithreading ? 1 : 0;
        int ok = (avcodec_open2(probe, c, NULL) == 0);
        const char* why = ok ? NULL : "open";
        if (ok) {
            AVPacket* tp = av_packet_alloc();
            int fed = 0;
            while (tp && !fed && av_read_frame(s->fmt, tp) >= 0) {
                if (tp->stream_index == s->vstream) {
                    if (avcodec_send_packet(probe, tp) < 0) { ok = 0; why = "packet"; }
                    fed = 1;
                }
                av_packet_unref(tp);
            }
            if (tp) av_packet_free(&tp);
            av_seek_frame(s->fmt, s->vstream, 0, AVSEEK_FLAG_BACKWARD);
        }
        avcodec_free_context(&probe);
        if (ok) { dec = c; usingHw = 1; hwName = cand[i]; }
        else ashenlog("OPEN note=hw_unavailable clip=%s decoder=%s at=%s",
                      Ashen_ClipName(path), cand[i], why);
    }
    if (!dec) dec = swDec;
    if (!dec) { Ashen_CloseSession(s); return NULL; }

    s->hwPixFmt = AV_PIX_FMT_NONE;
    if (hwMode == 2 && !usingHw) {
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* c = avcodec_get_hw_config(dec, i);
            if (!c) break;
            if ((c->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                c->device_type == AV_HWDEVICE_TYPE_D3D11VA) { s->hwPixFmt = c->pix_fmt; break; }
        }
        if (s->hwPixFmt != AV_PIX_FMT_NONE &&
            av_hwdevice_ctx_create(&s->hwDev, AV_HWDEVICE_TYPE_D3D11VA, NULL, NULL, 0) < 0) {
            s->hwDev = NULL; s->hwPixFmt = AV_PIX_FMT_NONE;
        }
        if (s->hwPixFmt != AV_PIX_FMT_NONE) {
            s->hwSw = av_frame_alloc();
            if (!s->hwSw) { s->hwPixFmt = AV_PIX_FMT_NONE; }
        }
        if (s->hwPixFmt == AV_PIX_FMT_NONE)
            ashenlog("OPEN note=d3d11va_unavailable clip=%s codec=%s",
                     Ashen_ClipName(path), avcodec_get_name(st->codecpar->codec_id));
    }

    s->ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(s->ctx, st->codecpar);
            if (mxfForced) s->ctx->codec_id = cid_;
    s->ctx->thread_count = Ashen_Cfg().disableMultithreading ? 1 : 0;
    if (dec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
        s->ctx->thread_type = FF_THREAD_SLICE;
    if (s->hwPixFmt != AV_PIX_FMT_NONE && s->hwDev) {
        s->ctx->opaque        = s;
        s->ctx->hw_device_ctx = av_buffer_ref(s->hwDev);
        s->ctx->get_format    = Ashen_GetFormat;
    }

    if (avcodec_open2(s->ctx, dec, NULL) < 0) {
        if (s->hwPixFmt != AV_PIX_FMT_NONE) {
            ashenlog("OPEN note=d3d11va_open_failed clip=%s -> software", Ashen_ClipName(path));
            avcodec_free_context(&s->ctx);
            s->hwPixFmt = AV_PIX_FMT_NONE;
            if (s->hwSw)  av_frame_free(&s->hwSw);
            if (s->hwDev) av_buffer_unref(&s->hwDev);
            s->ctx = avcodec_alloc_context3(dec);
            avcodec_parameters_to_context(s->ctx, st->codecpar);
            if (mxfForced) s->ctx->codec_id = cid_;
            s->ctx->thread_count = Ashen_Cfg().disableMultithreading ? 1 : 0;
            if (dec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
                s->ctx->thread_type = FF_THREAD_SLICE;
            if (avcodec_open2(s->ctx, dec, NULL) < 0) { Ashen_CloseSession(s); return NULL; }
        }
        else if (usingHw) {
            avcodec_free_context(&s->ctx);
            dec = avcodec_find_decoder(cid_);
            usingHw = 0;
            s->ctx = avcodec_alloc_context3(dec);
            avcodec_parameters_to_context(s->ctx, st->codecpar);
            if (mxfForced) s->ctx->codec_id = cid_;
            s->ctx->thread_count = Ashen_Cfg().disableMultithreading ? 1 : 0;
            if (dec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
                s->ctx->thread_type = FF_THREAD_SLICE;
            if (avcodec_open2(s->ctx, dec, NULL) < 0) { Ashen_CloseSession(s); return NULL; }
        } else {
            Ashen_CloseSession(s); return NULL;
        }
    }

    const AVCodecDescriptor* desc = avcodec_descriptor_get(cid_);
    s->intraOnly = Ashen_IsIntraCodec(cid_) ||
                   (desc && (desc->props & AV_CODEC_PROP_INTRA_ONLY)) ? 1 : 0;

    if (s->intraOnly && st->codecpar->codec_id != AV_CODEC_ID_CFHD &&
        !Ashen_IsUncompressed(st->codecpar->codec_id)) {
        int answer = s->intraOnly;
        if (Ashen_FactsAcquire(path, &answer)) {
            s->intraOnly = answer;
            scanCached = 1;
        } else {
            double _tp = ashen_now_ms();
            AVPacket* probe = av_packet_alloc();
            int scanned = 0, sawNonKey = 0;
            const int64_t kScanByteCap = 64ll * 1024 * 1024;
            int64_t bytes = 0;
            while (probe && scanned < 120 && bytes < kScanByteCap &&
                   av_read_frame(s->fmt, probe) >= 0) {
                if (probe->stream_index == s->vstream) {
                    ++scanned; bytes += probe->size;
                    if (!(probe->flags & AV_PKT_FLAG_KEY)) { sawNonKey = 1; av_packet_unref(probe); break; }
                }
                av_packet_unref(probe);
            }
            if (probe) av_packet_free(&probe);
            if (sawNonKey) {
                s->intraOnly = 0;
                ashenlog("OPEN note=gop_detected clip=%s reason=non_keyframe_packet", Ashen_ClipName(path));
            }
            probeMs = ashen_now_ms() - _tp;
            scanPackets = scanned; scanBytes = bytes;
            Ashen_FactsPublish(path, s->intraOnly);
            av_seek_frame(s->fmt, s->vstream, 0, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(s->ctx);
        }
    }

    if (st->codecpar->codec_id == AV_CODEC_ID_CFHD) {
        if (CFHD_OpenDecoder(&s->cfhd, NULL) == CFHD_ERROR_OKAY) {
            s->useCineForm = 1;
            ashenlog("OPEN note=cineform_native clip=%s", Ashen_ClipName(path));
        }
    }

    s->pkt   = av_packet_alloc();
    s->frame = av_frame_alloc();
    s->keep  = av_frame_alloc();
    if (!s->pkt || !s->frame || !s->keep) { Ashen_CloseSession(s); return NULL; }
    s->usingHw = usingHw;
    s->codecId = (int)cid_;
    s->lowres = 0;
    s->maxLowres = (st->codecpar->codec_id == AV_CODEC_ID_JPEG2000 && dec->max_lowres > 0)
                   ? (int)dec->max_lowres : 0;
    {
        AVRational fwr = av_guess_frame_rate(s->fmt, st, NULL);
        int fps = (fwr.num && fwr.den) ? (int)((double)fwr.num / (double)fwr.den + 0.5) : 25;
        if (fps < 1) fps = 25;
        if (!s->intraOnly && !s->useCineForm)                       s->fwdFrames = fps;
        else if (Ashen_IsUncompressed(st->codecpar->codec_id))      s->fwdFrames = 1;
        else                                                        s->fwdFrames = 3;
    }
    strncpy_s(s->clip, sizeof(s->clip), Ashen_ClipName(path), _TRUNCATE);
    strncpy_s(s->codec, sizeof(s->codec), avcodec_get_name(st->codecpar->codec_id), _TRUNCATE);
    if (importerID >= 0) {
        std::lock_guard<std::mutex> g(g_statMutex);
        ClipStat* c = Ashen_Stat(importerID);
        strncpy_s(c->clip, sizeof(c->clip), s->clip, _TRUNCATE);
        strncpy_s(c->codec, sizeof(c->codec), s->codec, _TRUNCATE);
        strncpy_s(c->decoder, sizeof(c->decoder), dec->name, _TRUNCATE);
        c->hw = usingHw; c->intra = s->intraOnly;
    }
    ashenlog("OPEN clip=%s codec=%s imp=%d decoder=%s hw=%d intra=%d size=%dx%d "
             "total=%.1f open=%.1f info=%.1f scan=%s%.1f pkts=%d mb=%.1f hwaccel=%s",
           s->clip, s->codec, importerID, dec->name, usingHw ? 1 : 0, s->intraOnly,
           st->codecpar->width, st->codecpar->height,
           ashen_now_ms() - _tOpen, openMs, infoMs,
           scanCached ? "shared:" : "", probeMs, scanPackets,
           (double)scanBytes / (1024.0 * 1024.0),
           (s->hwPixFmt != AV_PIX_FMT_NONE) ? "d3d11va" : (usingHw && hwName ? hwName : "none"));
    return s;
}

static AshenSession* Ashen_AcquireSession(int importerID, const char* path,
                                          int frameIndex)
{
    std::unique_lock<std::mutex> lk(g_poolMutex);
    auto& vec = g_pool[importerID];
    if (!vec.empty() && strcmp(vec[0]->path, path) != 0) {
        int closed = 0, doomed = 0;
        Ashen_RetirePool(vec, &closed, &doomed);
        if (doomed)
            ashenlog("CLIPCHANGE imp=%d closed=%d busy_doomed=%d", importerID, closed, doomed);
        FrameCache_Release(importerID);
    }
    const unsigned long long myTicket = g_poolTicket[importerID]++;
    unsigned long long*      serve    = &g_poolServe[importerID];
    struct AshenTicketGuard {
        unsigned long long*      serve;
        std::condition_variable* cv;
        bool                     done;
        void release() { if (!done) { done = true; ++(*serve); cv->notify_all(); } }
        ~AshenTicketGuard() { release(); }
    } ticket = { serve, &g_poolCv, false };

    for (;;) {
        if (myTicket != *serve) { g_poolCv.wait(lk); continue; }
        {
            AshenSession* best = NULL;
            long long bestRank = 0;
            for (AshenSession* s : vec) {
                if (s->busy) continue;
                long long rank;
                if (s->lastFrame == INT_MIN) {
                    rank = 3000000000ll;
                } else {
                    long long w = (s->fwdFrames > 0) ? s->fwdFrames : 1;
                    if (w > 1 && (long long)vec.size() > w) w = (long long)vec.size();
                    long long gap = (long long)frameIndex - (long long)s->lastFrame;
                    rank = (gap >= 0 && gap <= w) ? gap
                                                  : 2000000000ll + (gap < 0 ? -gap : gap);
                }
                if (!best || rank < bestRank) { best = s; bestRank = rank; }
            }
            if (best) {
                best->busy = true;
                best->poolSize = (int)vec.size();
                ticket.release();
                return best;
            }
        }
        int cap = (!vec.empty() && vec[0]->usingHw) ? Ashen_Cfg().hwSessions : 2;
        int& building = g_poolCreating[importerID];
        if ((int)vec.size() + building < cap) {
            ++building;
            lk.unlock();
            AshenSession* s = Ashen_CreateSession(importerID, path);
            lk.lock();
            --building;
            if (!s) return NULL;
            s->busy = true;
            s->poolSize = (int)vec.size() + 1;
            vec.push_back(s);
            ticket.release();
            return s;
        }
        g_poolCv.wait(lk);
    }
}

static void Ashen_ReleaseSession(AshenSession* s)
{
    int doomed;
    {
        std::lock_guard<std::mutex> lk(g_poolMutex);
        s->busy  = false;
        doomed   = s->doomed;
    }
    g_poolCv.notify_all();
    if (doomed) Ashen_CloseSession(s);
}

static int Ashen_IsSoftwareSession(int importerID)
{
    std::lock_guard<std::mutex> lock(g_poolMutex);
    auto it = g_pool.find(importerID);
    return (it != g_pool.end() && !it->second.empty() && !it->second[0]->usingHw) ? 1 : 0;
}

static prMALError Ashen_ScaleFrameToDst(AshenSession* s, AVFrame* frame,
                                      int width, int height, int dstStride, char* dst,
                                      PrPixelFormat outFmt);

static int Ashen_CacheFrames()
{
    int v = Ashen_Cfg().cacheFrames;
    Ashen_EnvInt("ASHEN_CACHE_FRAMES", &v);
    return (v > 0) ? v : 0;
}

struct FrameCacheEntry {
    int fmt, w, h, rowBytes, pinned;
    std::vector<uint8_t> buf;
};
static std::map<long long, FrameCacheEntry*> g_fcMap;
static std::map<int, int>                    g_fcCenter;
static size_t                                g_fcBytes    = 0;
static int                                   g_fcMaxFrames = 0;
static size_t                                g_fcHits     = 0;
static size_t                                g_fcMisses   = 0;
static size_t                                g_fcEvicted  = 0;
static size_t                                g_fcRejects  = 0;
static std::mutex                            g_fcMutex;

static int Ashen_CacheEnabled()
{
    static int resolved = -1;
    if (resolved < 0) {
        g_fcMaxFrames = Ashen_CacheFrames();
        resolved = (g_fcMaxFrames > 0) ? 1 : 0;
    }
    return resolved;
}

static int Ashen_IsCacheableCodec(enum AVCodecID id)
{
    switch (id) {
        case AV_CODEC_ID_PRORES: return 1;
        case AV_CODEC_ID_VP9:    return 1;
        case AV_CODEC_ID_VP8:    return 1;
        default:                 return 0;
    }
}

static inline long long Ashen_FcKey(int importerID, int frame)
{
    return ((long long)(unsigned)importerID << 32) | (unsigned)frame;
}

static int FrameCache_GetInto(AshenSession* s, int frame, int width, int height,
                              int dstStride, char* dstBGRA, PrPixelFormat outFmt)
{
    if (!Ashen_CacheEnabled()) return 0;
    if (!dstBGRA) return 0;

    FrameCacheEntry* e = NULL;
    {
        std::lock_guard<std::mutex> g(g_fcMutex);
        g_fcCenter[s->importerID] = frame;
        auto it = g_fcMap.find(Ashen_FcKey(s->importerID, frame));
        if (it == g_fcMap.end()) { ++g_fcMisses; return 0; }
        e = it->second;
        if (e->fmt != (int)outFmt || e->w != width || e->h != height) {
            ++g_fcMisses; ++g_fcRejects; return 0;
        }
        ++g_fcHits;
        e->pinned++;
    }
    const uint8_t* src = e->buf.data();
    if (dstStride == e->rowBytes) {
        memcpy(dstBGRA, src, (size_t)e->rowBytes * (size_t)e->h);
    } else {
        char* d = dstBGRA;
        for (int y = 0; y < e->h; ++y) { memcpy(d, src, e->rowBytes); d += dstStride; src += e->rowBytes; }
    }
    { std::lock_guard<std::mutex> g(g_fcMutex); e->pinned--; }
    return 1;
}

static void FrameCache_PutConverted(int importerID, int frame, int width, int height,
                                    int dstStride, const char* dstBGRA, PrPixelFormat outFmt)
{
    if (!Ashen_CacheEnabled()) return;
    if (!dstBGRA || width <= 0 || height <= 0) return;
    int rowBytes = Ashen_NativeRowBytes(outFmt, width);
    if (rowBytes <= 0 || dstStride < rowBytes) return;
    size_t sz = (size_t)rowBytes * (size_t)height;

    FrameCacheEntry* e = new FrameCacheEntry();
    e->fmt = (int)outFmt; e->w = width; e->h = height;
    e->rowBytes = rowBytes; e->pinned = 0;
    e->buf.resize(sz);
    if (dstStride == rowBytes) {
        memcpy(e->buf.data(), dstBGRA, sz);
    } else {
        uint8_t* d = e->buf.data(); const char* srow = dstBGRA;
        for (int y = 0; y < height; ++y) { memcpy(d, srow, rowBytes); d += rowBytes; srow += dstStride; }
    }

    long long key = Ashen_FcKey(importerID, frame);
    std::lock_guard<std::mutex> g(g_fcMutex);
    auto it = g_fcMap.find(key);
    if (it != g_fcMap.end()) {
        if (it->second->pinned) { delete e; return; }
        g_fcBytes -= it->second->buf.size();
        delete it->second; g_fcMap.erase(it);
    }
    g_fcMap[key] = e; g_fcBytes += sz;

    int center = g_fcCenter.count(importerID) ? g_fcCenter[importerID] : frame;
    for (;;) {
        int live = 0;
        auto worst = g_fcMap.end(); long long worstDist = -1;
        for (auto i = g_fcMap.begin(); i != g_fcMap.end(); ++i) {
            if ((int)(i->first >> 32) != importerID) continue;
            ++live;
            if (i->second->pinned) continue;
            long long d = llabs((long long)(int)(unsigned)i->first - (long long)center);
            if (d > worstDist) { worstDist = d; worst = i; }
        }
        if (live <= g_fcMaxFrames || worst == g_fcMap.end()) break;
        g_fcBytes -= worst->second->buf.size();
        delete worst->second; g_fcMap.erase(worst); ++g_fcEvicted;
    }
}

static void FrameCache_Release(int importerID)
{
    std::lock_guard<std::mutex> g(g_fcMutex);
    for (auto it = g_fcMap.begin(); it != g_fcMap.end(); ) {
        if ((int)(it->first >> 32) == importerID) {
            g_fcBytes -= it->second->buf.size();
            delete it->second; it = g_fcMap.erase(it);
        } else { ++it; }
    }
    g_fcCenter.erase(importerID);
}

static void Ashen_LogCacheStats(const char* clip, const char* codec)
{
    std::lock_guard<std::mutex> g(g_fcMutex);
    size_t tot = g_fcHits + g_fcMisses;
    ashenlog("SUM clip=%s codec=%s framecache hits=%zu misses=%zu hitrate=%.1f%% "
             "evicted=%zu rejected=%zu frames=%zu bytes=%zu window=%d",
             clip, codec, g_fcHits, g_fcMisses,
             tot ? (100.0 * (double)g_fcHits / (double)tot) : 0.0,
             g_fcEvicted, g_fcRejects, g_fcMap.size(), g_fcBytes, g_fcMaxFrames);
}

static int Ashen_NextFrame(AshenSession* s)
{
    for (;;) {
        int r = avcodec_receive_frame(s->ctx, s->frame);
        if (r == 0) {
            if (s->hwPixFmt != AV_PIX_FMT_NONE) {
                AVFrame* sysf = Ashen_ToSystemMemory(s, s->frame);
                if (!sysf) return -1;
                if (sysf != s->frame) av_frame_move_ref(s->frame, sysf);
            }
            return 1;
        }
        if (r == AVERROR_EOF) return 0;
        if (r == AVERROR(EAGAIN)) {
            int rd = av_read_frame(s->fmt, s->pkt);
            if (rd >= 0) {
                if (s->pkt->stream_index == s->vstream)
                    avcodec_send_packet(s->ctx, s->pkt);
                av_packet_unref(s->pkt);
            } else {
                avcodec_send_packet(s->ctx, NULL);
            }
            continue;
        }
        return -1;
    }
}

static struct SwsContext* Ashen_MakeSws(int srcW, int srcH, enum AVPixelFormat srcFmt,
                                        int dstW, int dstH, enum AVPixelFormat dstFmt,
                                        int srcColorRange, int srcColorspace)
{
    const AVPixFmtDescriptor* sd = av_pix_fmt_desc_get(srcFmt);
    const int dstIsRGB = (dstFmt == AV_PIX_FMT_BGRA || dstFmt == AV_PIX_FMT_BGRA64LE);
    const int srcSubsampled = sd && !(sd->flags & AV_PIX_FMT_FLAG_RGB) &&
                              (sd->log2_chroma_w > 0 || sd->log2_chroma_h > 0);

    int flags = SWS_BILINEAR | SWS_ACCURATE_RND;
    if (dstFmt == AV_PIX_FMT_BGRA && srcSubsampled && Ashen_Cfg().fullChroma)
        flags |= SWS_FULL_CHR_H_INT;

    struct SwsContext* ctx = sws_getContext(srcW, srcH, srcFmt,
                                            dstW, dstH, dstFmt,
                                            flags, NULL, NULL, NULL);
    if (!ctx) return NULL;
    int srcRange = (srcColorRange == AVCOL_RANGE_JPEG) ? 1 : 0;
    int dstRange = dstIsRGB ? 1 : 0;
    const int coeff = Ashen_ColorspaceCoeff(srcColorspace);
    sws_setColorspaceDetails(ctx, sws_getCoefficients(coeff), srcRange,
        sws_getCoefficients(coeff), dstRange, 0, 1 << 16, 1 << 16);
    return ctx;
}

static prMALError Ashen_ScaleFrameToDst(AshenSession* s, AVFrame* frame,
                                      int width, int height, int dstStride, char* dstBGRA,
                                      PrPixelFormat outFmt)
{
    const double _tcvt = ashen_now_ms();
    if (s) s->cvtMs = 0.0;
    #define ASHEN_CVT_DONE() do { if (s) s->cvtMs = ashen_now_ms() - _tcvt; } while (0)
    if (!frame->data[0] || frame->width <= 0 || frame->height <= 0) return imFrameNotFound;

    enum AVPixelFormat dstAv = Ashen_AvFormatFor(outFmt);
    int nativeRow = Ashen_NativeRowBytes(outFmt, width);
    if (dstStride <= 0) dstStride = nativeRow;

    #define Ashen_DST_IS_RGB(f) ((f) == AV_PIX_FMT_BGRA || (f) == AV_PIX_FMT_BGRA64LE)
    int flipDst = Ashen_DST_IS_RGB(dstAv) && Ashen_FLIP_VERTICAL;

    if (dstAv != AV_PIX_FMT_NONE && (enum AVPixelFormat)frame->format == dstAv &&
        frame->width == width && frame->height == height) {
        const uint8_t* src = frame->data[0];
        int srcStride = frame->linesize[0];
        int copy = (srcStride < nativeRow) ? srcStride : nativeRow;
        for (int y = 0; y < height; ++y) {
            int dy = flipDst ? (height - 1 - y) : y;
            memcpy(dstBGRA + (size_t)dstStride * dy, src + (size_t)srcStride * y, (size_t)copy);
        }
        ASHEN_CVT_DONE();
        return malNoError;
    }

    if (dstAv == AV_PIX_FMT_NONE) dstAv = AV_PIX_FMT_BGRA;
    flipDst = Ashen_DST_IS_RGB(dstAv) && Ashen_FLIP_VERTICAL;
    #undef Ashen_DST_IS_RGB

    if (!s->sws || s->swsW != width || s->swsH != height ||
        s->swsSrcFmt != frame->format || s->swsSrcW != frame->width ||
        s->swsSrcH != frame->height || s->swsDstFmt != (int)dstAv) {
        if (s->sws) sws_freeContext(s->sws);
        s->sws = Ashen_MakeSws(frame->width, frame->height, (AVPixelFormat)frame->format,
                               width, height, dstAv, frame->color_range, frame->colorspace);
        s->swsW = width; s->swsH = height; s->swsSrcFmt = frame->format;
        s->swsSrcW = frame->width; s->swsSrcH = frame->height; s->swsDstFmt = (int)dstAv;
    }
    if (!s->sws) return imFrameNotFound;

    uint8_t* dstData[4]; int dstLines[4];
    memset(dstData, 0, sizeof(dstData)); memset(dstLines, 0, sizeof(dstLines));
    if (flipDst) {
        dstData[0]  = (uint8_t*)dstBGRA + (size_t)dstStride * (height - 1);
        dstLines[0] = -dstStride;
    } else {
        dstData[0]  = (uint8_t*)dstBGRA;
        dstLines[0] = dstStride;
    }
    sws_scale(s->sws, frame->data, frame->linesize, 0, frame->height, dstData, dstLines);
    ASHEN_CVT_DONE();
    #undef ASHEN_CVT_DONE
    return malNoError;
}

static int Ashen_DecodeIntraExact(AshenSession* s, int64_t targetPts, int64_t tol)
{
    int sentAny = 0;
    for (;;) {
        int r = avcodec_receive_frame(s->ctx, s->frame);
        if (r == 0) {
            if (s->frame->pts == AV_NOPTS_VALUE || s->frame->pts >= targetPts - tol) {
                if (s->hwPixFmt != AV_PIX_FMT_NONE) {
                    AVFrame* sysf = Ashen_ToSystemMemory(s, s->frame);
                    if (!sysf) return 0;
                    if (sysf != s->frame) av_frame_move_ref(s->frame, sysf);
                }
                return 1;
            }
            continue;
        }
        if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) return 0;
        int rd = av_read_frame(s->fmt, s->pkt);
        if (rd < 0) {
            if (r == AVERROR_EOF) return 0;
            avcodec_send_packet(s->ctx, NULL);
            continue;
        }
        if (s->pkt->stream_index != s->vstream) { av_packet_unref(s->pkt); continue; }
        int64_t pts = (s->pkt->pts != AV_NOPTS_VALUE) ? s->pkt->pts : s->pkt->dts;
        if (!sentAny && pts != AV_NOPTS_VALUE && pts < targetPts - tol) { av_packet_unref(s->pkt); continue; }
        sentAny = 1;
        avcodec_send_packet(s->ctx, s->pkt);
        av_packet_unref(s->pkt);
    }
}

static int64_t Ashen_MatchTolerance(AVStream* st, AVRational fr)
{
    AVRational invFps; invFps.num = 1; invFps.den = (fr.num ? fr.num : 25);
    int64_t oneFrame = av_rescale_q((int64_t)(fr.den ? fr.den : 1), invFps, st->time_base);
    return (oneFrame > 1) ? oneFrame / 2 : 0;
}

static int64_t Ashen_ForwardWindow(AshenSession* s, AVStream* st, AVRational fr)
{
    AVRational oneS; oneS.num = 1; oneS.den = 1;
    int64_t oneSec = av_rescale_q(1, oneS, st->time_base);
    if (!s->intraOnly && !s->useCineForm) return oneSec;

    AVRational invFps; invFps.num = 1; invFps.den = (fr.num ? fr.num : 25);
    int64_t oneFrame = av_rescale_q((int64_t)(fr.den ? fr.den : 1), invFps, st->time_base);
    if (oneFrame <= 0) oneFrame = 1;

    int frames = Ashen_IsUncompressed(st->codecpar->codec_id) ? 1 : 3;
    if (frames > 1 && s->poolSize > frames) frames = s->poolSize;
    int64_t w = oneFrame * frames;
    return (w < oneSec) ? w : oneSec;
}

static int Ashen_SetLowres(AshenSession* s, int lowres)
{
    if (lowres < 0) lowres = 0;
    if (s->maxLowres <= 0 || lowres == s->lowres) return 1;
    if (lowres > s->maxLowres) lowres = s->maxLowres;
    AVStream* st = s->fmt->streams[s->vstream];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) return 0;
    AVCodecContext* nctx = avcodec_alloc_context3(dec);
    if (!nctx) return 0;
    avcodec_parameters_to_context(nctx, st->codecpar);
    nctx->thread_count = Ashen_Cfg().disableMultithreading ? 1 : 0;
    if (dec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
        nctx->thread_type = FF_THREAD_SLICE;
    nctx->lowres = lowres;
    if (avcodec_open2(nctx, dec, NULL) < 0) { avcodec_free_context(&nctx); return 0; }
    avcodec_free_context(&s->ctx);
    s->ctx = nctx;
    s->lowres = lowres;
    s->lastPts = INT64_MIN;
    s->lastFrame = INT_MIN;
    s->atEof = 0;
    ashenlog("Ashen_SetLowres: importer=%d lowres=%d (decoded %dx%d)", s->importerID, lowres,
           st->codecpar->width >> lowres, st->codecpar->height >> lowres);
    return 1;
}

static prMALError Ashen_DecodeWithSession(AshenSession* s, csSDK_int32 frameIndex,
                                        int width, int height, int dstStride, char* dstBGRA,
                                        int lowresHint, PrPixelFormat outFmt)
{
    AVStream* st = s->fmt->streams[s->vstream];
    AVRational fr = Ashen_FrameRate(s->fmt, st);
    AVRational invFps; invFps.num = 1; invFps.den = (fr.num ? fr.num : 25);
    int64_t targetPts = av_rescale_q((int64_t)frameIndex * (fr.den ? fr.den : 1),
                                     invFps, st->time_base);

    int64_t tol = Ashen_MatchTolerance(st, fr);

    double t0 = ashen_now_ms();
    int callNo = ++g_decodeCalls;

    if (s->maxLowres > 0) {
        int wantLR = lowresHint < 0 ? 0 : (lowresHint > s->maxLowres ? s->maxLowres : lowresHint);
        if (wantLR != s->lowres) Ashen_SetLowres(s, wantLR);
    }

    if (s->useCineForm && s->cfhd && Ashen_CineFormCanEmit(outFmt)) {
        int64_t fwdT = Ashen_ForwardWindow(s, st, fr);
        int didSeek = 0;
        if (s->lastPts == INT64_MIN || targetPts < s->lastPts || (targetPts - s->lastPts) > fwdT) {
            if (av_seek_frame(s->fmt, s->vstream, targetPts, AVSEEK_FLAG_BACKWARD) < 0)
                av_seek_frame(s->fmt, s->vstream, 0, AVSEEK_FLAG_BACKWARD);
            didSeek = 1;
        }
        int found = 0;
        for (;;) {
            int rd = av_read_frame(s->fmt, s->pkt);
            if (rd < 0) break;
            if (s->pkt->stream_index != s->vstream) { av_packet_unref(s->pkt); continue; }
            int64_t pts = (s->pkt->pts != AV_NOPTS_VALUE) ? s->pkt->pts : s->pkt->dts;
            if (pts != AV_NOPTS_VALUE && pts < targetPts - tol) { av_packet_unref(s->pkt); continue; }
            found = 1; break;
        }
        prMALError result = imFrameNotFound;
        if (found) {
            int pitch = (dstStride > 0) ? dstStride : width * 4;
            CFHD_PixelFormat want = CFHD_PIXEL_FORMAT_UNKNOWN;
            switch (outFmt) {
                case PrPixelFormat_UYVY_422_8u_601: case PrPixelFormat_UYVY_422_8u_709:
                    want = CFHD_PIXEL_FORMAT_2VUY; break;
                case PrPixelFormat_YUYV_422_8u_601: case PrPixelFormat_YUYV_422_8u_709:
                    want = CFHD_PIXEL_FORMAT_YUY2; break;
                case PrPixelFormat_BGRA_4444_8u:
                    want = CFHD_PIXEL_FORMAT_BGRA; break;
                case PrPixelFormat_BGRA_4444_16u:
                    want = CFHD_PIXEL_FORMAT_B64A; break;
                default: break;
            }
            if (want == CFHD_PIXEL_FORMAT_UNKNOWN) {
                ashenlog("CineForm refuse frame %d unsupported_outfmt=%d (LISTS OUT OF SYNC)",
                         (int)frameIndex, (int)outFmt);
                av_packet_unref(s->pkt);
                goto cfhd_done;
            }
            if (!s->cfhdPrepared || s->cfhdW != width || s->cfhdH != height ||
                s->cfhdFmt != (int)want) {
                int aw = 0, ah = 0; CFHD_PixelFormat af = want;
                {
                    CFHD_PixelFormat can[32]; int nCan = 0, listed = 0;
                    if (CFHD_GetOutputFormats(s->cfhd, s->pkt->data, s->pkt->size,
                                              can, 32, &nCan) == CFHD_ERROR_OKAY) {
                        for (int i = 0; i < nCan; ++i) if (can[i] == want) { listed = 1; break; }
                    }
                    if (!listed) {
                        ashenlog("CineForm refuse frame %d fmt_not_offered=%d -> ffmpeg",
                                 (int)frameIndex, (int)want);
                        av_packet_unref(s->pkt);
                        s->cfhdPrepared = 0;
                        goto cfhd_done;
                    }
                }
                CFHD_Error pe = CFHD_PrepareToDecode(s->cfhd, width, height, want,
                                     CFHD_DECODED_RESOLUTION_FULL, CFHD_DECODING_FLAGS_NONE,
                                     s->pkt->data, s->pkt->size, &aw, &ah, &af);
                if (pe != CFHD_ERROR_OKAY) {
                    ashenlog("CineForm prepare error %d frame %d want=%dx%d fmt=%d",
                             (int)pe, (int)frameIndex, width, height, (int)want);
                    av_packet_unref(s->pkt);
                    s->cfhdPrepared = 0;
                    goto cfhd_done;
                }
                s->cfhdPrepared = 1; s->cfhdW = width; s->cfhdH = height;
                s->cfhdOutW = aw;    s->cfhdOutH = ah;  s->cfhdFmt = (int)want;
            }
            if (s->cfhdOutW > width || s->cfhdOutH > height ||
                pitch < Ashen_NativeRowBytes(outFmt, s->cfhdOutW)) {
                ashenlog("CineForm refuse frame %d out=%dx%d dst=%dx%d pitch=%d need=%d fmt=%d",
                         (int)frameIndex, s->cfhdOutW, s->cfhdOutH, width, height, pitch,
                         Ashen_NativeRowBytes(outFmt, s->cfhdOutW), (int)outFmt);
                av_packet_unref(s->pkt);
                goto cfhd_done;
            }
            CFHD_Error e = CFHD_DecodeSample(s->cfhd, s->pkt->data, s->pkt->size, dstBGRA, pitch);
            if (e == CFHD_ERROR_OKAY && want == CFHD_PIXEL_FORMAT_B64A) {
                double _tsw = ashen_now_ms();
                for (int y = 0; y < height; ++y) {
                    unsigned short* px = (unsigned short*)(dstBGRA + (size_t)y * pitch);
                    for (int x = 0; x < width; ++x, px += 4) {
                        unsigned short a = px[0], r = px[1], g = px[2], b = px[3];
                        px[0] = b; px[1] = g; px[2] = r; px[3] = a;
                    }
                }
                ashenlog2("CFHD-SWIZZLE frame=%d dur=%.1f", (int)frameIndex,
                          ashen_now_ms() - _tsw);
            }
            if (e == CFHD_ERROR_OKAY) result = malNoError;
            else ashenlog("CineForm decode error %d frame %d", (int)e, (int)frameIndex);
            av_packet_unref(s->pkt);
        }
cfhd_done:
        s->lastPts = (result == malNoError) ? targetPts : INT64_MIN;
        s->lastFrame = (result == malNoError) ? (int)frameIndex : INT_MIN;
        double el = ashen_now_ms() - t0;
        ashenlog2("DEC clip=%s codec=%s imp=%d f=%d path=cfhd seek=%d ok=%d dur=%.1f",
               s->clip, s->codec, s->importerID, (int)frameIndex, didSeek,
               (result == malNoError) ? 1 : 0, el);
        if (result == malNoError) Ashen_StatSample(s->importerID, didSeek ? "seek" : "fwd", el);
        else                      Ashen_StatFail(s->importerID);
        return result;
    }

    if (s->intraOnly) {
        int cacheable = Ashen_IsCacheableCodec(st->codecpar->codec_id);
        if (cacheable && FrameCache_GetInto(s, frameIndex, width, height, dstStride, dstBGRA, outFmt)) {
            double el = ashen_now_ms() - t0;
            ashenlog2("DEC clip=%s codec=%s imp=%d f=%d path=fc ok=1 dur=%.1f",
                   s->clip, s->codec, s->importerID, (int)frameIndex, el);
            Ashen_StatSample(s->importerID, "fc", el);
            return malNoError;
        }
        int64_t fwdT = Ashen_ForwardWindow(s, st, fr);
        int didSeek = 0;
        if (s->lastPts == INT64_MIN || targetPts < s->lastPts || (targetPts - s->lastPts) > fwdT) {
            if (av_seek_frame(s->fmt, s->vstream, targetPts, AVSEEK_FLAG_BACKWARD) < 0)
                av_seek_frame(s->fmt, s->vstream, 0, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(s->ctx);
            didSeek = 1;
        }
        int got = Ashen_DecodeIntraExact(s, targetPts, tol);
        double el = ashen_now_ms() - t0;
        ashenlog2("DEC clip=%s codec=%s imp=%d f=%d path=intra seek=%d ok=%d lowres=%d "
               "size=%dx%d out=%d retpts=%lld dur=%.1f",
               s->clip, s->codec, s->importerID, (int)frameIndex, didSeek, got, s->lowres,
               width, height, (int)outFmt,
               (long long)(got ? s->frame->pts : -1), el);
        if (got) Ashen_StatSample(s->importerID, didSeek ? "seek" : "fwd", el);
        else     Ashen_StatFail(s->importerID);
        if (got) {
            if (Ashen_ScaleFrameToDst(s, s->frame, width, height, dstStride, dstBGRA, outFmt) == malNoError) {
                if (cacheable && s->lowres == 0)
                    FrameCache_PutConverted(s->importerID, (int)frameIndex, width, height,
                                            dstStride, dstBGRA, outFmt);
                s->lastPts = (s->frame->pts != AV_NOPTS_VALUE) ? s->frame->pts : targetPts;
                s->lastFrame = (int)frameIndex;
                return malNoError;
            }
        }
        s->lastPts = INT64_MIN;
        s->lastFrame = INT_MIN;
        ashenlog("Ashen_DecodeFrameBGRA: intra frame %d not found in %s", frameIndex, s->path);
        return imFrameNotFound;
    }

    AVRational oneSec; oneSec.num = 1; oneSec.den = 1;
    int64_t fwdThreshold = av_rescale_q(1, oneSec, st->time_base);
    const int seekFlags = 0;

    #define Ashen_SEEK_TO(ts) do { \
        if (avformat_seek_file(s->fmt, s->vstream, INT64_MIN, (ts), (ts), seekFlags) < 0 && \
            av_seek_frame(s->fmt, s->vstream, (ts), AVSEEK_FLAG_BACKWARD) < 0) \
            av_seek_frame(s->fmt, s->vstream, 0, AVSEEK_FLAG_BACKWARD); \
        avcodec_flush_buffers(s->ctx); \
        av_frame_unref(s->keep); \
        s->atEof = 0; \
        didSeek = 1; \
    } while (0)

    int gopCacheable = Ashen_IsCacheableCodec(st->codecpar->codec_id);
    if (gopCacheable && FrameCache_GetInto(s, frameIndex, width, height, dstStride, dstBGRA, outFmt)) {
        double elc = ashen_now_ms() - t0;
        ashenlog2("DEC clip=%s codec=%s imp=%d f=%d path=fc ok=1 dur=%.1f",
               s->clip, s->codec, s->importerID, (int)frameIndex, elc);
        Ashen_StatSample(s->importerID, "fc", elc);
        return malNoError;
    }

    int64_t oneFrameTb = av_rescale_q((int64_t)(fr.den ? fr.den : 1), invFps, st->time_base);
    if (oneFrameTb <= 0) oneFrameTb = 1;

    int didSeek = 0;
    int wantSeek;
    const AVIndexEntry* kfEntry = NULL;
    {
        int ie = av_index_search_timestamp(st, targetPts, AVSEEK_FLAG_BACKWARD);
        kfEntry = (ie >= 0) ? avformat_index_get_entry(st, ie) : NULL;
    }
    if (s->lastPts == INT64_MIN || s->atEof || targetPts < s->lastPts) {
        wantSeek = 1;
    } else {
        wantSeek = kfEntry ? (kfEntry->timestamp > s->lastPts)
                           : ((targetPts - s->lastPts) > fwdThreshold);
    }
    int64_t synth = (s->lastPts != INT64_MIN) ? s->lastPts + oneFrameTb : 0;
    if (wantSeek) {
        Ashen_SEEK_TO(targetPts);
        synth = kfEntry ? kfEntry->timestamp : 0;
    }

    prMALError result = imFrameNotFound;
    int got = 0;
    int64_t gotTs = AV_NOPTS_VALUE;
    for (int pass = 0; pass < 2 && !got; ++pass) {
        int rc;
        while ((rc = Ashen_NextFrame(s)) == 1) {
            int64_t ts = s->frame->pts;
            if (ts == AV_NOPTS_VALUE) { ts = synth; synth += oneFrameTb; }
            if (ts >= targetPts - tol) { got = 1; gotTs = ts; break; }
            av_frame_unref(s->keep);
            av_frame_ref(s->keep, s->frame);
        }
        if (got) break;

        if (rc == 0) {
            s->atEof = 1;
            if (s->keep->data[0]) { got = 2; break; }
        } else {
            ashenlog("DEC-ERR clip=%s codec=%s imp=%d f=%d reason=decode_error_before_target",
                     s->clip, s->codec, s->importerID, (int)frameIndex);
        }
        if (didSeek) break;
        Ashen_SEEK_TO(targetPts);
        synth = kfEntry ? kfEntry->timestamp : 0;
    }
    #undef Ashen_SEEK_TO

    AVFrame* frame = (got == 2) ? s->keep : s->frame;
    if (got && (!frame->data[0] || frame->width <= 0 || frame->height <= 0)) {
        ashenlog("DEC-ERR clip=%s codec=%s imp=%d f=%d reason=empty_frame size=%dx%d",
               s->clip, s->codec, s->importerID, (int)frameIndex, frame->width, frame->height);
        got = 0;
    }

    double el = ashen_now_ms() - t0;
    ashenlog2("DEC clip=%s codec=%s imp=%d f=%d path=gop seek=%d ok=%d "
           "size=%dx%d out=%d dur=%.1f cvt=%.1f",
           s->clip, s->codec, s->importerID, (int)frameIndex, didSeek, got,
           width, height, (int)outFmt, el, s->cvtMs);
    if (got) Ashen_StatSample(s->importerID, didSeek ? "seek" : "fwd", el);
    else     Ashen_StatFail(s->importerID);

    if (got) {
        if (Ashen_ScaleFrameToDst(s, frame, width, height, dstStride, dstBGRA, outFmt) == malNoError) {
            if (gopCacheable && got == 1)
                FrameCache_PutConverted(s->importerID, (int)frameIndex, width, height,
                                        dstStride, dstBGRA, outFmt);
            s->lastPts = (frame->pts != AV_NOPTS_VALUE) ? frame->pts
                       : (gotTs != AV_NOPTS_VALUE ? gotTs : targetPts);
            s->lastFrame = (int)frameIndex;
            result = malNoError;
        }
    }
    if (result != malNoError) {
        ashenlog("Ashen_DecodeFrameBGRA: frame %d not found in %s", frameIndex, s->path);
        s->lastPts = INT64_MIN;
        s->lastFrame = INT_MIN;
    }
    return result;
}

static int Ashen_DemoteToSoftware(AshenSession* s)
{
    if (!s || !s->usingHw) return 0;
    const AVCodec* sw = avcodec_find_decoder((enum AVCodecID)s->codecId);
    if (!sw) return 0;

    AVStream* st = s->fmt->streams[s->vstream];
    AVCodecContext* nctx = avcodec_alloc_context3(sw);
    if (!nctx) return 0;
    avcodec_parameters_to_context(nctx, st->codecpar);
    nctx->codec_id     = (enum AVCodecID)s->codecId;
    nctx->thread_count = Ashen_Cfg().disableMultithreading ? 1 : 0;
    if (sw->capabilities & AV_CODEC_CAP_SLICE_THREADS)
        nctx->thread_type = FF_THREAD_SLICE;
    if (avcodec_open2(nctx, sw, NULL) < 0) { avcodec_free_context(&nctx); return 0; }

    ashenlog("DEMOTE clip=%s imp=%d %s -> software after %d consecutive failures",
             s->clip, s->importerID, s->codec, s->decFails);

    if (s->ctx) avcodec_free_context(&s->ctx);
    s->ctx      = nctx;
    s->usingHw  = 0;
    s->decFails = 0;
    s->hwPixFmt = AV_PIX_FMT_NONE;
    if (s->hwSw)  av_frame_free(&s->hwSw);
    if (s->hwDev) av_buffer_unref(&s->hwDev);
    av_seek_frame(s->fmt, s->vstream, 0, AVSEEK_FLAG_BACKWARD);
    s->lastPts = INT64_MIN;
    s->lastFrame = INT_MIN;
    s->atEof   = 0;
    av_frame_unref(s->keep);
    return 1;
}

static prMALError Ashen_DecodeFrameBGRA(int importerID, const prUTF16Char* path16,
                                      csSDK_int32 frameIndex, int width, int height,
                                      int dstStride, char* dstBGRA, int lowresHint,
                                      PrPixelFormat outFmt)
{
    char path[2048];
    Ashen_Utf16ToUtf8(path16, path, sizeof(path));
    AshenSession* s = Ashen_AcquireSession(importerID, path, (int)frameIndex);
    if (!s) return imBadFile;
    prMALError r = Ashen_DecodeWithSession(s, frameIndex, width, height, dstStride, dstBGRA, lowresHint, outFmt);

    if (r != malNoError && s->usingHw && ++s->decFails >= 3) {
        if (Ashen_DemoteToSoftware(s))
            r = Ashen_DecodeWithSession(s, frameIndex, width, height, dstStride, dstBGRA, lowresHint, outFmt);
    }
    if (r == malNoError) s->decFails = 0;

    Ashen_ReleaseSession(s);
    return r;
}

struct CachedFrame { std::vector<uint8_t> bgra; int w, h; };

struct PrefetchEngine {
    char        path[2048];
    int         importerID;
    int         width, height;
    size_t      frameBytes;
    size_t      maxFrames;
    int         depth;

    std::map<int, CachedFrame*> cache;
    std::list<int>              lru;
    std::mutex                  cacheMtx;

    int64_t                     base;
    bool                        baseValid;
    std::thread                 worker;
    std::mutex                  ctlMtx;
    std::condition_variable      cv;
    bool                        stop;
};

static std::map<int, PrefetchEngine*> g_prefetch;
static std::mutex                     g_prefetchMutex;

static void PrefetchWorker(PrefetchEngine* pe)
{
    AshenSession* ps = Ashen_CreateSession(-1, pe->path);
    if (!ps) return;
    AVStream*  st  = ps->fmt->streams[ps->vstream];
    AVRational fr  = av_guess_frame_rate(ps->fmt, st, NULL);
    AVRational inv; inv.num = 1; inv.den = fr.num ? fr.num : 25;
    int readPos = -1;

    for (;;)
    {
        int64_t base;
        {
            std::unique_lock<std::mutex> lk(pe->ctlMtx);
            pe->cv.wait(lk, [&]{ return pe->stop || (pe->baseValid && readPos < (int)pe->base + pe->depth); });
            if (pe->stop) break;
            base = pe->base;
        }
        if (readPos < (int)base || readPos > (int)base + pe->depth) {
            int64_t tp = av_rescale_q((int64_t)base * (fr.den ? fr.den : 1), inv, st->time_base);
            if (avformat_seek_file(ps->fmt, ps->vstream, INT64_MIN, tp, tp, AVSEEK_FLAG_ANY) < 0)
                av_seek_frame(ps->fmt, ps->vstream, 0, AVSEEK_FLAG_BACKWARD);
            readPos = (int)base;
        }
        for (int i = 0; i < 8 && readPos < (int)base + pe->depth; ++i) {
            int rd = av_read_frame(ps->fmt, ps->pkt);
            if (rd < 0) { readPos = (int)base + pe->depth; break; }
            if (ps->pkt->stream_index == ps->vstream) ++readPos;
            av_packet_unref(ps->pkt);
            std::lock_guard<std::mutex> lk(pe->ctlMtx);
            if (pe->stop || pe->base != base) break;
        }
    }
    Ashen_CloseSession(ps);
}

static int Prefetch_TryGet(int importerID, int frameNum, int w, int h, char* dst)
{
    PrefetchEngine* pe = NULL;
    { std::lock_guard<std::mutex> g(g_prefetchMutex); auto it = g_prefetch.find(importerID); if (it != g_prefetch.end()) pe = it->second; }
    if (!pe) return 0;

    int hit = 0;
    {
        std::lock_guard<std::mutex> ck(pe->cacheMtx);
        auto it = pe->cache.find(frameNum);
        if (it != pe->cache.end() && it->second->w == w && it->second->h == h) {
            memcpy(dst, it->second->bgra.data(), (size_t)w * h * 4);
            hit = 1;
        }
    }
    ashenlog("prefetch %s frame=%d", hit ? "HIT" : "miss", frameNum);
    return hit;
}

#define Ashen_ENABLE_PREFETCH 0

static void Prefetch_Advance(int importerID, const prUTF16Char* path16, int frameNum, int w, int h)
{
#if !Ashen_ENABLE_PREFETCH
    (void)importerID; (void)path16; (void)frameNum; (void)w; (void)h;
    return;
#else
    bool sw = false, readBound = false;
    {
        std::lock_guard<std::mutex> g(g_sessionMutex);
        auto it = g_sessions.find(importerID);
        if (it != g_sessions.end() && !it->second->usingHw) {
            sw = true;
            readBound = it->second->ctx && Ashen_IsUncompressed(it->second->ctx->codec_id);
        }
    }
    if (!sw || !readBound || w <= 0 || h <= 0) return;

    PrefetchEngine* pe = NULL;
    {
        std::lock_guard<std::mutex> g(g_prefetchMutex);
        auto it = g_prefetch.find(importerID);
        if (it != g_prefetch.end()) pe = it->second;
        else {
            pe = new PrefetchEngine();
            pe->importerID = importerID;
            Ashen_Utf16ToUtf8(path16, pe->path, sizeof(pe->path));
            pe->width = w; pe->height = h;
            pe->frameBytes = (size_t)w * h * 4;
            pe->maxFrames = 0;
            pe->depth     = 64;
            pe->base = frameNum; pe->baseValid = true; pe->stop = false;
            pe->worker = std::thread(PrefetchWorker, pe);
            g_prefetch[importerID] = pe;
            ashenlog("Prefetch: engine created importerID=%d depth=%d maxFrames=%zu", importerID, pe->depth, pe->maxFrames);
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lk(pe->ctlMtx);
        pe->width = w; pe->height = h;
        pe->base = frameNum; pe->baseValid = true;
    }
    pe->cv.notify_one();
#endif
}

static void Prefetch_Release(int importerID)
{
    PrefetchEngine* pe = NULL;
    {
        std::lock_guard<std::mutex> g(g_prefetchMutex);
        auto it = g_prefetch.find(importerID);
        if (it != g_prefetch.end()) { pe = it->second; g_prefetch.erase(it); }
    }
    if (!pe) return;
    { std::lock_guard<std::mutex> lk(pe->ctlMtx); pe->stop = true; }
    pe->cv.notify_all();
    if (pe->worker.joinable()) pe->worker.join();
    for (auto& kv : pe->cache) delete kv.second;
    delete pe;
}

static const int64_t kAshenAudioPreroll = 4096;

struct AshenAudioStream {
    int              index;
    AVCodecContext*  ctx;
    SwrContext*      swr;
    AVRational       tb;
    int              chOffset;
    int              channels;
    std::vector<float> carry;
    int64_t          carryStart;
    int              carryCount;
    int              anchored;
    int              eof;
};

struct AshenAudio {
    char             path[2048];
    AVFormatContext* fmt;
    std::vector<AshenAudioStream> st;
    int              channels;
    int              sampleRate;
    int64_t          cursor;
    int              positioned;
    AVPacket*        pkt;
    AVFrame*         frame;
};
static std::map<int, AshenAudio*> g_audio;

static void Ashen_CloseAudio(AshenAudio* a)
{
    if (!a) return;
    for (AshenAudioStream& s : a->st) {
        if (s.swr) swr_free(&s.swr);
        if (s.ctx) avcodec_free_context(&s.ctx);
    }
    if (a->frame) av_frame_free(&a->frame);
    if (a->pkt)   av_packet_free(&a->pkt);
    if (a->fmt)   avformat_close_input(&a->fmt);
    delete a;
}

static AshenAudio* Ashen_GetAudio(int importerID, const char* path)
{
    auto it = g_audio.find(importerID);
    if (it != g_audio.end()) {
        if (strcmp(it->second->path, path) == 0) return it->second;
        Ashen_CloseAudio(it->second);
        g_audio.erase(it);
    }

    AshenAudio* a = new AshenAudio();
    a->fmt = NULL; a->pkt = NULL; a->frame = NULL;
    a->channels = 0; a->sampleRate = 0; a->cursor = 0; a->positioned = 0;
    strncpy_s(a->path, sizeof(a->path), path, _TRUNCATE);

    if (avformat_open_input(&a->fmt, path, NULL, NULL) < 0) { Ashen_CloseAudio(a); return NULL; }
    if (avformat_find_stream_info(a->fmt, NULL) < 0) { Ashen_CloseAudio(a); return NULL; }

    for (unsigned i = 0; i < a->fmt->nb_streams && a->channels < 8; ++i) {
        AVStream* ast = a->fmt->streams[i];
        if (ast->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        int srcCh = ast->codecpar->ch_layout.nb_channels;
        if (srcCh <= 0) continue;

        if (a->sampleRate == 0) a->sampleRate = ast->codecpar->sample_rate;
        else if (ast->codecpar->sample_rate != a->sampleRate) {
            ashenlog("AUD-WARN clip=%s stream=%u rate=%d != %d -> stream ignored",
                     Ashen_ClipName(path), i, ast->codecpar->sample_rate, a->sampleRate);
            continue;
        }

        const AVCodec* dec = avcodec_find_decoder(ast->codecpar->codec_id);
        if (!dec) continue;

        AshenAudioStream s;
        s.index = (int)i; s.tb = ast->time_base; s.eof = 0;
        s.carryStart = 0; s.carryCount = 0; s.anchored = 0;
        s.chOffset = a->channels;
        s.channels = srcCh;
        if (s.chOffset + s.channels > 8) s.channels = 8 - s.chOffset;

        s.ctx = avcodec_alloc_context3(dec);
        if (!s.ctx) continue;
        avcodec_parameters_to_context(s.ctx, ast->codecpar);
        if (avcodec_open2(s.ctx, dec, NULL) < 0) { avcodec_free_context(&s.ctx); continue; }

        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, s.channels);
        s.swr = NULL;
        if (swr_alloc_set_opts2(&s.swr, &outLayout, AV_SAMPLE_FMT_FLTP, a->sampleRate,
                                &s.ctx->ch_layout, s.ctx->sample_fmt, s.ctx->sample_rate,
                                0, NULL) < 0 || swr_init(s.swr) < 0) {
            av_channel_layout_uninit(&outLayout);
            if (s.swr) swr_free(&s.swr);
            avcodec_free_context(&s.ctx);
            continue;
        }
        av_channel_layout_uninit(&outLayout);

        a->channels += s.channels;
        a->st.push_back(s);
        ashenlog("AUD-OPEN clip=%s stream=%u codec=%s src_ch=%d -> out_ch=%d..%d rate=%d",
                 Ashen_ClipName(path), i, dec->name, srcCh,
                 s.chOffset, s.chOffset + s.channels - 1, a->sampleRate);
    }

    if (a->st.empty()) { Ashen_CloseAudio(a); return NULL; }

    for (unsigned i = 0; i < a->fmt->nb_streams; ++i) {
        int mine = 0;
        for (const AshenAudioStream& s : a->st) if (s.index == (int)i) { mine = 1; break; }
        if (!mine) a->fmt->streams[i]->discard = AVDISCARD_ALL;
    }

    a->pkt = av_packet_alloc();
    a->frame = av_frame_alloc();
    if (!a->pkt || !a->frame) { Ashen_CloseAudio(a); return NULL; }
    g_audio[importerID] = a;
    ashenlog("AUD-OPEN clip=%s streams=%d channels=%d rate=%d",
             Ashen_ClipName(path), (int)a->st.size(), a->channels, a->sampleRate);
    return a;
}

static void Ashen_AudioCarryAppend(AshenAudio* a, AshenAudioStream& s,
                                   int64_t absStart, float** planes, int nSamples,
                                   int64_t keepFrom)
{
    if (nSamples <= 0) return;

    if (s.carryCount > 0 && keepFrom > s.carryStart) {
        int64_t drop = keepFrom - s.carryStart;
        if (drop >= s.carryCount) { s.carryStart += s.carryCount; s.carryCount = 0; }
        else {
            int rem = s.carryCount - (int)drop;
            for (int c = 0; c < s.channels; ++c) {
                float* base = &s.carry[(size_t)c * (s.carry.size() / (size_t)s.channels)];
                memmove(base, base + drop, (size_t)rem * sizeof(float));
            }
            s.carryStart += drop;
            s.carryCount = rem;
        }
    }
    if (absStart != s.carryStart + s.carryCount) {
        s.carryStart = absStart;
        s.carryCount = 0;
    }

    size_t needCap = (size_t)s.carryCount + (size_t)nSamples;
    size_t curCap  = s.carry.empty() ? 0 : s.carry.size() / (size_t)s.channels;
    if (needCap > curCap) {
        size_t newCap = needCap + 8192;
        std::vector<float> grown((size_t)s.channels * newCap, 0.f);
        for (int c = 0; c < s.channels; ++c)
            if (curCap)
                memcpy(&grown[(size_t)c * newCap], &s.carry[(size_t)c * curCap],
                       (size_t)s.carryCount * sizeof(float));
        s.carry.swap(grown);
        curCap = newCap;
    }
    for (int c = 0; c < s.channels; ++c)
        memcpy(&s.carry[(size_t)c * curCap + s.carryCount], planes[c],
               (size_t)nSamples * sizeof(float));
    s.carryCount += nSamples;
}

static int64_t Ashen_AudioCarryEnd(const AshenAudioStream& s)
{
    return s.carryStart + s.carryCount;
}

static int Ashen_AudioPump(AshenAudio* a, int64_t keepFrom)
{
    int rd = av_read_frame(a->fmt, a->pkt);
    if (rd < 0) {
        int any = 0;
        for (AshenAudioStream& s : a->st) {
            if (s.eof) continue;
            avcodec_send_packet(s.ctx, NULL);
            s.eof = 1; any = 1;
        }
        if (!any) return 0;
    } else {
        AshenAudioStream* target = NULL;
        for (AshenAudioStream& s : a->st)
            if (s.index == a->pkt->stream_index) { target = &s; break; }
        if (target) avcodec_send_packet(target->ctx, a->pkt);
        av_packet_unref(a->pkt);
        if (!target) return 1;
    }

    uint8_t* tmp[8] = {0};
    for (AshenAudioStream& s : a->st) {
        for (;;) {
            int r = avcodec_receive_frame(s.ctx, a->frame);
            if (r != 0) break;
            int64_t expected = Ashen_AudioCarryEnd(s);
            int64_t absStart = expected;
            if (a->frame->pts != AV_NOPTS_VALUE) {
                AVRational sr; sr.num = 1; sr.den = a->sampleRate;
                int64_t fromPts = av_rescale_q(a->frame->pts, s.tb, sr);
                if (!s.anchored || llabs(fromPts - expected) > (int64_t)a->sampleRate)
                    absStart = fromPts;
            }
            s.anchored = 1;
            int linesize = 0;
            if (av_samples_alloc(tmp, &linesize, s.channels, a->frame->nb_samples + 64,
                                 AV_SAMPLE_FMT_FLTP, 0) >= 0 && tmp[0]) {
                int outN = swr_convert(s.swr, tmp, a->frame->nb_samples + 64,
                                       (const uint8_t**)a->frame->data, a->frame->nb_samples);
                if (outN > 0)
                    Ashen_AudioCarryAppend(a, s, absStart, (float**)tmp, outN, keepFrom);
                av_freep(&tmp[0]);
            }
        }
    }
    return (rd >= 0) ? 1 : 1;
}

static void Ashen_ReadAudio(int importerID, const prUTF16Char* path16,
                          int64_t startPos, csSDK_uint32 size, int premChannels, float** buffer)
{
    char path[2048];
    Ashen_Utf16ToUtf8(path16, path, sizeof(path));

    if (premChannels < 1) premChannels = 1;
    if (premChannels > 8) premChannels = 8;

    #define Ashen_AUDIO_SILENCE() do { \
        for (int c = 0; c < premChannels; ++c) \
            if (buffer[c]) for (csSDK_uint32 i = 0; i < size; ++i) buffer[c][i] = 0.f; \
    } while (0)

    double t0 = ashen_now_ms();
    std::lock_guard<std::mutex> lock(g_sessionMutex);
    AshenAudio* a = Ashen_GetAudio(importerID, path);
    if (!a) {
        Ashen_AUDIO_SILENCE();
        ashenlog2("AUD imp=%d pos=%lld n=%u ch=%d src=none dur=%.1f",
                  importerID, (long long)startPos, size, premChannels, ashen_now_ms() - t0);
        return;
    }

    if (startPos < 0) startPos = 0;
    const int64_t wantEnd = startPos + (int64_t)size;

    int mustSeek = !a->positioned;
    if (!mustSeek) {
        for (AshenAudioStream& s : a->st) {
            if (startPos < s.carryStart) { mustSeek = 1; break; }
        }
        if (startPos > a->cursor + (int64_t)a->sampleRate) mustSeek = 1;
    }

    if (mustSeek) {
        int64_t seekTo = startPos - kAshenAudioPreroll;
        if (seekTo < 0) seekTo = 0;
        AVRational sr; sr.num = 1; sr.den = a->sampleRate;
        int ref = a->st[0].index;
        int64_t ts = av_rescale_q(seekTo, sr, a->st[0].tb);
        av_seek_frame(a->fmt, ref, ts, AVSEEK_FLAG_BACKWARD);
        for (AshenAudioStream& s : a->st) {
            avcodec_flush_buffers(s.ctx);
            swr_convert(s.swr, NULL, 0, NULL, 0);
            s.carryStart = 0; s.carryCount = 0; s.eof = 0;
            s.anchored = 0;
        }
        a->positioned = 1;
    }

    int guard = 0;
    for (;;) {
        int covered = 1;
        for (AshenAudioStream& s : a->st) {
            if (!s.eof && Ashen_AudioCarryEnd(s) < wantEnd) { covered = 0; break; }
        }
        if (covered) break;
        if (++guard > 100000) break;
        if (!Ashen_AudioPump(a, startPos)) break;
        int allEof = 1;
        for (AshenAudioStream& s : a->st) if (!s.eof) { allEof = 0; break; }
        if (allEof) {
            int done = 1;
            for (AshenAudioStream& s : a->st)
                if (Ashen_AudioCarryEnd(s) < wantEnd) { done = 1; break; }
            if (done) break;
        }
    }

    int filled = 0;
    std::vector<unsigned char> got((size_t)size, 0);
    for (int c = 0; c < premChannels; ++c)
        if (buffer[c]) for (csSDK_uint32 i = 0; i < size; ++i) buffer[c][i] = 0.f;

    for (AshenAudioStream& s : a->st) {
        size_t cap = s.carry.empty() ? 0 : s.carry.size() / (size_t)s.channels;
        if (!cap || s.carryCount <= 0) continue;
        for (int c = 0; c < s.channels; ++c) {
            int outCh = s.chOffset + c;
            if (outCh >= premChannels || !buffer[outCh]) continue;
            const float* src = &s.carry[(size_t)c * cap];
            for (csSDK_uint32 i = 0; i < size; ++i) {
                int64_t abs = startPos + (int64_t)i;
                int64_t off = abs - s.carryStart;
                if (off < 0 || off >= s.carryCount) continue;
                buffer[outCh][i] = src[off];
                if (!got[i]) { got[i] = 1; filled++; }
            }
        }
    }

    a->cursor = wantEnd;
    ashenlog2("AUD imp=%d pos=%lld n=%u ch=%d streams=%d seek=%d filled=%d dur=%.1f",
              importerID, (long long)startPos, size, premChannels,
              (int)a->st.size(), mustSeek, filled, ashen_now_ms() - t0);
    #undef Ashen_AUDIO_SILENCE
}

static void Ashen_ReleaseAudio(int importerID)
{
    std::lock_guard<std::mutex> lock(g_sessionMutex);
    auto it = g_audio.find(importerID);
    if (it != g_audio.end()) { Ashen_CloseAudio(it->second); g_audio.erase(it); }
}

static prMALError
Ashen_OnGetIndColorSpace(
	imStdParms			*stdParms,
	csSDK_size_t		index,
	imIndColorSpaceRec	*colorSpaceRec)
{
	if (!colorSpaceRec) return imOtherErr;
	if (index > 0) return imBadFormatIndex;

	AshenLocalH ldataH =
		reinterpret_cast<AshenLocalH>(colorSpaceRec->inPrivateData);
	if (!ldataH || !*ldataH) return imOtherErr;

	colorSpaceRec->outColorSpaceType = kPrSDKColorSpaceType_SEITags;
	colorSpaceRec->outSEICodesRec.colorPrimariesCode         = (*ldataH)->clip.colorPrimaries;
	colorSpaceRec->outSEICodesRec.transferCharacteristicCode = (*ldataH)->clip.colorTrc;
	colorSpaceRec->outSEICodesRec.matrixEquationsCode        = (*ldataH)->clip.colorMatrix;
	colorSpaceRec->outSEICodesRec.bitDepth                   = (*ldataH)->clip.mediaBitDepth;

	ashenlog("COLORSPACE imp=%d primaries=%d transfer=%d matrix=%d bits=%d",
	         (*ldataH)->importerID, (int)(*ldataH)->clip.colorPrimaries,
	         (int)(*ldataH)->clip.colorTrc, (int)(*ldataH)->clip.colorMatrix,
	         (int)(*ldataH)->clip.mediaBitDepth);
	return malNoError;
}

static void Ashen_InitLocalRec(AshenLocalH h)
{
	if (!h || !*h) return;
	memset(*h, 0, sizeof(AshenLocal));
	(*h)->importerID = -1;
	(*h)->fileRef    = imInvalidHandleValue;
}

static void Ashen_ScaleAndSampleSize(PrSDKTimeSuite* time, PrTime frameTicks,
                                     csSDK_int32* scale, csSDK_int32* sampleSize)
{
	PrTime tps = 0;
	time->GetTicksPerSecond(&tps);
	if (tps % frameTicks == 0) {
		*scale      = static_cast<csSDK_int32>(tps / frameTicks);
		*sampleSize = 1;
		return;
	}
	struct { PrVideoFrameRates rate; csSDK_int32 scale; } ntsc[] = {
		{ kVideoFrameRate_NTSC,    30000 },
		{ kVideoFrameRate_NTSC_HD, 60000 },
		{ kVideoFrameRate_24Drop,  24000 },
	};
	for (auto& r : ntsc) {
		PrTime t = 0;
		time->GetTicksPerVideoFrame(r.rate, &t);
		if (t == frameTicks) { *scale = r.scale; *sampleSize = 1001; }
	}
}

static void Ashen_SpreadRows(char* buf, csSDK_int32 rowBytes, int bytesPerPixel, int width, int height)
{
	const csSDK_int32 packed = width * bytesPerPixel;
	if (packed >= rowBytes) return;
	for (int y = height - 1; y >= 0; --y)
		memmove(buf + (size_t)y * rowBytes, buf + (size_t)y * packed, packed);
}

static prMALError Ashen_OnInit(imStdParms* stdParms, imImportInfoRec* info)
{
	info->setupOnDblClk           = kPrFalse;
	info->canSave                 = kPrFalse;
	info->canDelete               = kPrFalse;
	info->hasSourceSettingsEffect = kPrFalse;
	info->dontCache               = kPrFalse;
	info->hasSetup                = kPrFalse;
	info->keepLoaded              = kPrFalse;
	info->priority                = 100;
	info->canTrim                 = kPrFalse;
	info->canCalcSizes            = kPrFalse;
	if (stdParms->imInterfaceVer >= IMPORTMOD_VERSION_6)
		info->avoidAudioConform = kPrTrue;
	return imIsCacheable;
}

static prMALError Ashen_OnGetPrefs(imFileAccessRec8* access, imGetPrefsRec* prefs)
{
	if (prefs->prefsLength == 0)
		prefs->prefsLength = sizeof(AshenLocal);
	else
		prUTF16CharCopy(reinterpret_cast<AshenLocal*>(prefs->prefs)->fileName, access->filepath);
	return malNoError;
}

static AshenLocalH Ashen_NewLocal(imStdParms* stdParms)
{
	AshenLocalH h = reinterpret_cast<AshenLocalH>(stdParms->piSuites->memFuncs->newHandle(sizeof(AshenLocal)));
	Ashen_InitLocalRec(h);
	return h;
}

static prMALError Ashen_OnOpenFile(imStdParms* stdParms, imFileRef* ref, imFileOpenRec8* open)
{
	ashenlog("SDKOpenFile8 enter");

	AshenLocalH localH = reinterpret_cast<AshenLocalH>(open->privatedata);
	if (!localH) {
		localH = Ashen_NewLocal(stdParms);
		open->privatedata = localH;
	}

	const DWORD access = (open->inReadWrite == kPrOpenFileAccess_ReadWrite) ? GENERIC_WRITE : GENERIC_READ;
	(*localH)->fileRef = CreateFileW(open->fileinfo.filepath, access, FILE_SHARE_READ, NULL,
	                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if ((*localH)->fileRef == imInvalidHandleValue)
		return imBadFile;

	*ref = (*localH)->fileRef;
	open->fileinfo.fileref  = (*localH)->fileRef;
	open->fileinfo.filetype = 'SDK_';
	return malNoError;
}

static prMALError Ashen_OnQuietFile(imFileRef* ref, void* privateData)
{
	AshenLocalH localH = reinterpret_cast<AshenLocalH>(privateData);
	if (localH && *localH && (*localH)->importerID >= 0)
		Ashen_QuietImporter((*localH)->importerID);

	if (ref && *ref != imInvalidHandleValue) {
		CloseHandle(*ref);
		*ref = imInvalidHandleValue;
	}
	return malNoError;
}

static prMALError Ashen_OnCloseFile(imStdParms* stdParms, imFileRef* ref, void* privateData)
{
	AshenLocalH localH = reinterpret_cast<AshenLocalH>(privateData);

	if (ref && *ref != imInvalidHandleValue)
		Ashen_OnQuietFile(ref, privateData);

	if (localH && *localH && (*localH)->importerID >= 0) {
		Prefetch_Release((*localH)->importerID);
		FrameCache_Release((*localH)->importerID);
		Ashen_ReleaseImporter((*localH)->importerID);
		Ashen_ReleaseAudio((*localH)->importerID);
	}

	if (localH && *localH && (*localH)->BasicSuite) {
		SPBasicSuite* basic = (*localH)->BasicSuite;
		basic->ReleaseSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion);
		basic->ReleaseSuite(kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion);
		basic->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
		basic->ReleaseSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion);
		stdParms->piSuites->memFuncs->disposeHandle(reinterpret_cast<char**>(localH));
	}
	return malNoError;
}

static prMALError Ashen_OnGetIndPixelFormat(csSDK_size_t index, imIndPixelFormatRec* rec)
{
	AshenLocalH localH = reinterpret_cast<AshenLocalH>(rec->privatedata);
	switch (index) {
		case 0:
			rec->outPixelFormat = (*localH)->clip.pixelFormat;
			return malNoError;
		case 1:
			rec->outPixelFormat = PrPixelFormat_BGRA_4444_8u;
			return ((*localH)->clip.pixelFormat == PrPixelFormat_BGRA_4444_8u)
			       ? imBadFormatIndex
			       : malNoError;
		default:
			return imBadFormatIndex;
	}
}

static prMALError Ashen_OnGetTimeInfo(imTimeInfoRec8* rec)
{
	rec->orgtime[0]    = 0;
	rec->alttime[0]    = 0;
	rec->orgreel[0]    = 0;
	rec->altreel[0]    = 0;
	rec->logcomment[0] = 0;
	return malNoError;
}

static prMALError Ashen_OnAnalysis(imAnalysisRec* rec)
{
	ashenlog("SDKAnalysis enter");
	strncpy_s(rec->buffer, rec->buffersize, "Uncompressed 8-bit RGB", _TRUNCATE);
	return malNoError;
}

static prMALError Ashen_OnDataRateAnalysis(imDataRateAnalysisRec* rec)
{
	AshenLocalH localH = reinterpret_cast<AshenLocalH>(rec->privatedata);
	ashenlog("SDKDataRateAnalysis enter (buffer=%p)", rec->buffer);

	const AshenClip& clip = (*localH)->clip;
	if (!rec->buffer) {
		rec->buffersize = clip.numFrames * sizeof(imDataSample);
		return malNoError;
	}

	rec->baserate = 0;
	csSDK_int32 scale = 0, sampleSize = 0;
	Ashen_ScaleAndSampleSize((*localH)->TimeSuite, clip.frameRate, &scale, &sampleSize);

	imDataSample* sample = reinterpret_cast<imDataSample*>(rec->buffer);
	for (csSDK_uint32 i = 0; i < clip.numFrames; ++i, ++sample) {
		sample->sampledur  = sampleSize + 0x80000000;
		sample->samplesize = clip.width * clip.height * 4;
	}
	return malNoError;
}

static prMALError Ashen_OnGetIndFormat(csSDK_size_t index, imIndFormatRec* rec)
{
	const char* exts[] = { "ivf", "obu", "mp4", "mov", "m4v", "mkv", "webm", "avi", "mj2", "apv",
	                       "mxf" };
	if (index >= sizeof(exts) / sizeof(exts[0]))
		return imBadFormatIndex;

	rec->filetype         = 'SYMP';
	rec->canWriteTimecode = kPrFalse;
	rec->flags            = xfCanOpen + xfCanImport + xfIsMovie;
	strcpy_s(rec->FormatName, sizeof(rec->FormatName), IMPORTER_NAME);
	strcpy_s(rec->FormatShortName, sizeof(rec->FormatShortName), IMPORTER_NAME);
	strcpy_s(rec->PlatformExtension, sizeof(rec->PlatformExtension), exts[index]);
	return malNoError;
}

static prMALError Ashen_OnImportAudio(imImportAudioRec7* rec)
{
	AshenLocalH localH = reinterpret_cast<AshenLocalH>(rec->privateData);

	PrAudioSample start = rec->position;
	if (start < 0)
		start = (*localH)->audioPosition;

	Ashen_ReadAudio((*localH)->importerID, (*localH)->fileName, start, rec->size,
	                Ashen_ChannelCount((*localH)->clip.channelType), rec->buffer);

	(*localH)->audioPosition = start + rec->size;
	return malNoError;
}

static void Ashen_FillVideoInfo(AshenLocalH localH, imFileInfoRec8* info)
{
	const AshenClip& clip = (*localH)->clip;
	if (!clip.hasVideo) {
		info->hasVideo = kPrFalse;
		info->vidInfo.imageWidth  = 0;
		info->vidInfo.imageHeight = 0;
		return;
	}

	info->hasVideo              = kPrTrue;
	info->vidInfo.subType       = clip.videoSubtype;
	info->vidInfo.imageWidth    = clip.width;
	info->vidInfo.imageHeight   = clip.height;
	info->vidInfo.depth         = clip.depth;
	info->vidInfo.fieldType     = static_cast<char>(clip.fieldType);

	info->vidInfo.alphaType      = clip.hasAlpha ? alphaStraight : alphaNone;
	info->vidInfo.pixelAspectNum = clip.pixelAspectNum;
	info->vidInfo.pixelAspectDen = clip.pixelAspectDen;

	info->vidInfo.colorSpaceSupport = imColorSpaceSupport_Fixed;
	info->vidInfo.bitDepth          = clip.mediaBitDepth;

	info->accessModes                    = kRandomAccessImport;
	info->vidInfo.supportsAsyncIO        = kPrTrue;
	info->vidInfo.supportsGetSourceVideo = kPrTrue;

	Ashen_ScaleAndSampleSize((*localH)->TimeSuite, clip.frameRate, &info->vidScale, &info->vidSampleSize);
	info->vidDuration = clip.numFrames * info->vidSampleSize;
}

static prMALError Ashen_FillAudioInfo(AshenLocalH localH, imFileInfoRec8* info)
{
	const AshenClip& clip = (*localH)->clip;
	if (!clip.hasAudio) {
		info->hasAudio = kPrFalse;
		return malNoError;
	}

	info->hasAudio = kPrTrue;
	const int channels = Ashen_ChannelCount(clip.channelType);
	prMALError rc = malNoError;
	if (channels > 0)
		info->audInfo.numChannels = channels;
	else
		rc = imBadFile;
	info->audInfo.sampleRate = (float)clip.sampleRate;
	info->audInfo.sampleType = kPrAudioSampleType_32BitFloat;
	info->audDuration        = clip.numSampleFrames;
	return rc;
}

static prMALError Ashen_OnGetInfo(imStdParms* stdParms, imFileAccessRec8* access, imFileInfoRec8* info)
{
	ashenlog("SDKGetInfo8 enter");

	const double t0 = ashen_now_ms();
	double probeMs = 0.0, vidMs = 0.0, audMs = 0.0, t = 0.0;

	info->accessModes                    = kRandomAccessImport;
	info->vidInfo.supportsAsyncIO        = kPrFalse;
	info->vidInfo.supportsGetSourceVideo = kPrTrue;
	info->vidInfo.hasPulldown            = kPrFalse;
	info->hasDataRate                    = kPrTrue;

	AshenLocalH localH = reinterpret_cast<AshenLocalH>(info->privatedata);
	if (!localH) {
		localH = Ashen_NewLocal(stdParms);
		info->privatedata = localH;
	}
	stdParms->piSuites->memFuncs->lockHandle(reinterpret_cast<char**>(localH));

	t = ashen_now_ms();
	prMALError rc = Ashen_Probe(access->filepath, &(*localH)->clip);
	probeMs = ashen_now_ms() - t;

	if (rc != malNoError) {
		stdParms->piSuites->memFuncs->unlockHandle(reinterpret_cast<char**>(localH));
		ashenlog("GETINFO declined total=%.1f probe=%.1f", ashen_now_ms() - t0, probeMs);
		return rc;
	}

	(*localH)->memFuncs   = stdParms->piSuites->memFuncs;
	(*localH)->BasicSuite = stdParms->piSuites->utilFuncs->getSPBasicSuite();
	if (SPBasicSuite* basic = (*localH)->BasicSuite) {
		basic->AcquireSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion, (const void**)&(*localH)->PPixCreatorSuite);
		basic->AcquireSuite(kPrSDKPPixCacheSuite,   kPrSDKPPixCacheSuiteVersion,   (const void**)&(*localH)->PPixCacheSuite);
		basic->AcquireSuite(kPrSDKPPixSuite,        kPrSDKPPixSuiteVersion,        (const void**)&(*localH)->PPixSuite);
		basic->AcquireSuite(kPrSDKTimeSuite,        kPrSDKTimeSuiteVersion,        (const void**)&(*localH)->TimeSuite);
	}
	(*localH)->audioPosition = 0;

	t = ashen_now_ms();
	Ashen_FillVideoInfo(localH, info);
	vidMs = ashen_now_ms() - t;

	t = ashen_now_ms();
	rc = Ashen_FillAudioInfo(localH, info);
	audMs = ashen_now_ms() - t;

	(*localH)->importerID = info->vidInfo.importerID;
	prUTF16CharCopy((*localH)->fileName, access->filepath);

	stdParms->piSuites->memFuncs->unlockHandle(reinterpret_cast<char**>(localH));

	ashenlog("GETINFO total=%.1f probe=%.1f video=%.1f audio=%.1f rc=%d",
	         ashen_now_ms() - t0, probeMs, vidMs, audMs, (int)rc);
	return rc;
}

static prMALError Ashen_OnPreferredFrameSize(imPreferredFrameSizeRec* rec)
{
	if (rec->inIndex != 0)
		return imOtherErr;
	AshenLocalH localH = reinterpret_cast<AshenLocalH>(rec->inPrivateData);
	rec->outWidth  = (*localH)->clip.width;
	rec->outHeight = (*localH)->clip.height;
	return malNoError;
}

static prMALError Ashen_ReadFrameToBuffer(AshenLocalH localH, csSDK_int32 frame,
                                          const imFrameFormat* format, char* buffer, int lowresHint)
{
	const int w  = format->inFrameWidth;
	const int h  = format->inFrameHeight;
	const int id = (*localH)->importerID;

	if (Prefetch_TryGet(id, frame, w, h, buffer)) {
		Prefetch_Advance(id, (*localH)->fileName, frame, w, h);
		return malNoError;
	}

	prMALError r = Ashen_DecodeFrameBGRA(id, (*localH)->fileName, frame, w, h,
	                                     0 , buffer,
	                                     lowresHint, format->inPixelFormat);
	if (r == malNoError)
		Prefetch_Advance(id, (*localH)->fileName, frame, w, h);
	return r;
}

static prMALError Ashen_OnGetSourceVideo(imSourceVideoRec* rec)
{
	AshenLocalH localH = reinterpret_cast<AshenLocalH>(rec->inPrivateData);
	const AshenClip& clip = (*localH)->clip;
	const csSDK_int32 frame = static_cast<csSDK_int32>(rec->inFrameTime / clip.frameRate);

	const double t0 = ashen_now_ms();

	imFrameFormat fmt;
	fmt.inFrameWidth  = 0;
	fmt.inFrameHeight = 0;
	fmt.inPixelFormat = PrPixelFormat_BGRA_4444_8u;

	if (rec->inFrameFormats && rec->inNumFrameFormats > 0) {
		csSDK_int32 pick = -1;
		for (csSDK_int32 i = 0; i < rec->inNumFrameFormats; ++i) {
			const PrPixelFormat pf = rec->inFrameFormats[i].inPixelFormat;
			if (pf == PrPixelFormat_BGRA_4444_8u || pf == PrPixelFormat_Any) { pick = i; break; }
		}
		if (pick < 0) {
			pick = 0;
			ashenlog("SDKGetSourceVideo: frame %d - host offered %d format(s), none BGRA "
			         "(first=%d) -> producing BGRA anyway",
			         (int)frame, (int)rec->inNumFrameFormats, (int)rec->inFrameFormats[0].inPixelFormat);
		}
		fmt.inFrameWidth  = rec->inFrameFormats[pick].inFrameWidth;
		fmt.inFrameHeight = rec->inFrameFormats[pick].inFrameHeight;
	}

	if (fmt.inFrameWidth <= 0)  fmt.inFrameWidth  = clip.width;
	if (fmt.inFrameHeight <= 0) fmt.inFrameHeight = clip.height;

	if (clip.width  > 0 && fmt.inFrameWidth  > clip.width)  fmt.inFrameWidth  = clip.width;
	if (clip.height > 0 && fmt.inFrameHeight > clip.height) fmt.inFrameHeight = clip.height;

	prMALError rc = (*localH)->PPixCacheSuite->GetFrameFromCache((*localH)->importerID, 0, frame, 1,
	                                                             &fmt, rec->outFrame, NULL, NULL);

	ashenlog("GSV frame=%d cache=%s getrc=%d ourfmt=%d nfmt=%d fmt0=%d want=%dx%d quality=%d intent=%d ratio=%.2f rate=%.2f",
	         (int)frame, (rc == suiteError_NoError) ? "HIT" : "miss", (int)rc, (int)fmt.inPixelFormat,
	         (int)rec->inNumFrameFormats,
	         (rec->inFrameFormats && rec->inNumFrameFormats > 0) ? (int)rec->inFrameFormats[0].inPixelFormat : -1,
	         (int)fmt.inFrameWidth, (int)fmt.inFrameHeight,
	         (int)rec->inQuality,
	         (int)rec->inRenderContext.inIntent,
	         rec->inRenderContext.inPlaybackRatio,
	         rec->inRenderContext.inPlaybackRate);

	if (rc != suiteError_NoError) {
		prRect bounds;
		prSetRect(&bounds, 0, 0, fmt.inFrameWidth, fmt.inFrameHeight);
		(*localH)->PPixCreatorSuite->CreatePPix(rec->outFrame, PrPPixBufferAccess_ReadWrite, fmt.inPixelFormat, &bounds);
		if (!*rec->outFrame) {
			AshenBufferFailLog((*localH)->importerID, (int)frame, fmt.inFrameWidth, fmt.inFrameHeight, "createppix");
			return imMemErr;
		}
		char* buffer = NULL;
		(*localH)->PPixSuite->GetPixels(*rec->outFrame, PrPPixBufferAccess_ReadWrite, &buffer);
		if (!buffer) {
			AshenBufferFailLog((*localH)->importerID, (int)frame, fmt.inFrameWidth, fmt.inFrameHeight, "getpixels");
			(*localH)->PPixSuite->Dispose(*rec->outFrame);
			*rec->outFrame = NULL;
			return imMemErr;
		}

		const int lowresHint = Ashen_LowresHint(rec->inRenderContext.inIntent, rec->inRenderContext.inPlaybackRatio);

		rc = Ashen_ReadFrameToBuffer(localH, frame, &fmt, buffer, lowresHint);

		if (rc != malNoError) {
			ashenlog("SDKGetSourceVideo: frame %d decode failed (%d) -> not caching", (int)frame, (int)rc);
			return rc;
		}

		csSDK_int32 rowBytes = 0;
		(*localH)->PPixSuite->GetRowBytes(*rec->outFrame, &rowBytes);
		Ashen_SpreadRows(buffer, rowBytes, 4, fmt.inFrameWidth, fmt.inFrameHeight);

		prSuiteError addRc = (*localH)->PPixCacheSuite->AddFrameToCache((*localH)->importerID, 0,
		                                                                *rec->outFrame, frame, NULL, NULL);
		ashenlog("ADD-CACHE frame=%d importerID=%d rc=%d fmt=%d %dx%d",
		         (int)frame, (int)(*localH)->importerID, (int)addRc,
		         (int)fmt.inPixelFormat, (int)fmt.inFrameWidth, (int)fmt.inFrameHeight);
	}

	ashenlog("GSV-DONE frame=%d total=%.1fms", (int)frame, ashen_now_ms() - t0);
	return rc;
}

prMALError AshenDecodeToBuffer(int importerID, const prUTF16Char* path16,
							   csSDK_int32 frameIndex, int width, int height,
							   int dstStride, char* dst, int lowresHint,
							   PrPixelFormat outFmt)
{
	return Ashen_DecodeFrameBGRA(importerID, path16, frameIndex, width, height,
								 dstStride, dst, lowresHint, outFmt);
}

int AshenLowresHintFor(int intent, double playbackRatio)
{
	return Ashen_LowresHint(intent, playbackRatio);
}

static prMALError Ashen_OnCreateAsyncImporter(imStdParms* stdParms, imAsyncImporterCreationRec* rec)
{
	rec->outAsyncEntry       = xAsyncImportEntry;
	rec->outAsyncPrivateData = reinterpret_cast<void*>(
		new AshenAsyncImporter(stdParms, reinterpret_cast<AshenLocalH>(rec->inPrivateData)));
	return malNoError;
}

PREMPLUGENTRY DllExport xImportEntry(csSDK_int32 selector, imStdParms* stdParms, void* param1, void* param2)
{
	Ashen_LoadFFmpegFromPluginDir();
	prMALError result = imUnsupported;

	ashenlog3("SEL n=%d", (int)selector);

	const double t0 = ashen_now_ms();

	switch (selector) {
		case imInit:
			result = Ashen_OnInit(stdParms, reinterpret_cast<imImportInfoRec*>(param1));
			break;
		case imGetPrefs8:
			result = Ashen_OnGetPrefs(reinterpret_cast<imFileAccessRec8*>(param1),
			                          reinterpret_cast<imGetPrefsRec*>(param2));
			break;
		case imGetInfo8:
			result = Ashen_OnGetInfo(stdParms, reinterpret_cast<imFileAccessRec8*>(param1),
			                         reinterpret_cast<imFileInfoRec8*>(param2));
			break;
		case imImportAudio7:
			result = Ashen_OnImportAudio(reinterpret_cast<imImportAudioRec7*>(param2));
			break;
		case imOpenFile8:
			result = Ashen_OnOpenFile(stdParms, reinterpret_cast<imFileRef*>(param1),
			                          reinterpret_cast<imFileOpenRec8*>(param2));
			break;
		case imQuietFile:
			result = Ashen_OnQuietFile(reinterpret_cast<imFileRef*>(param1), param2);
			break;
		case imCloseFile:
			result = Ashen_OnCloseFile(stdParms, reinterpret_cast<imFileRef*>(param1), param2);
			break;
		case imGetTimeInfo8:
			result = Ashen_OnGetTimeInfo(reinterpret_cast<imTimeInfoRec8*>(param2));
			break;
		case imAnalysis:
			result = Ashen_OnAnalysis(reinterpret_cast<imAnalysisRec*>(param2));
			break;
		case imDataRateAnalysis:
			result = Ashen_OnDataRateAnalysis(reinterpret_cast<imDataRateAnalysisRec*>(param2));
			break;
		case imGetIndFormat:
			result = Ashen_OnGetIndFormat(reinterpret_cast<csSDK_size_t>(param1),
			                              reinterpret_cast<imIndFormatRec*>(param2));
			break;
		case imGetMetaData:
		case imSetMetaData:
			result = malNoError;
			break;
		case imGetIndColorSpace:
			result = Ashen_OnGetIndColorSpace(stdParms, reinterpret_cast<csSDK_size_t>(param1),
			                             reinterpret_cast<imIndColorSpaceRec*>(param2));
			break;
		case imGetIndPixelFormat:
			result = Ashen_OnGetIndPixelFormat(reinterpret_cast<csSDK_size_t>(param1),
			                                   reinterpret_cast<imIndPixelFormatRec*>(param2));
			break;
		case imGetSupports8:
			result = malSupports8;
			break;
		case imGetPreferredFrameSize:
			result = Ashen_OnPreferredFrameSize(reinterpret_cast<imPreferredFrameSizeRec*>(param1));
			break;
		case imGetSourceVideo:
			result = Ashen_OnGetSourceVideo(reinterpret_cast<imSourceVideoRec*>(param2));
			break;
		case imCreateAsyncImporter:
			result = Ashen_OnCreateAsyncImporter(stdParms, reinterpret_cast<imAsyncImporterCreationRec*>(param1));
			break;
		case imPerformSourceSettingsCommand:
			result = malNoError;
			break;
		case imShutdown:
			break;
	}

	const double ms = ashen_now_ms() - t0;
	if (ms >= 1.0)
		ashenlog2("SEL n=%d dur=%.1f rc=%d", (int)selector, ms, (int)result);
	return result;
}
