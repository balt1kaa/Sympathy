#ifndef PREMIERE_API_H
#define PREMIERE_API_H

#include <windows.h>

#pragma pack(push, 1)

typedef int                 csSDK_int32;
typedef unsigned int        csSDK_uint32;
typedef short               csSDK_int16;
typedef unsigned short      csSDK_uint16;
typedef __int64             csSDK_int64;
typedef unsigned __int64    csSDK_size_t;
typedef csSDK_int32         prBool;
typedef csSDK_int32         prMALError;
typedef csSDK_int32         prSuiteError;
typedef int                 SPErr;
typedef csSDK_uint32        PrFourCC;
typedef csSDK_int64         PrTime;
typedef csSDK_int64         PrAudioSample;
typedef wchar_t             prUTF16Char;
typedef char*               PrMemoryPtr;
typedef PrMemoryPtr*        PrMemoryHandle;
typedef void*               imFileRef;
typedef csSDK_int32         PrPixelFormat;
typedef csSDK_int32         PrRenderQuality;
typedef csSDK_int32         PrVideoFrameRates;
typedef csSDK_int32         PrPPixBufferAccess;
typedef csSDK_int32         PrFileOpenAccess;
typedef csSDK_int32         PrAudioSampleType;
typedef csSDK_int32         imRenderIntent;

const prBool kPrTrue  = 1;
const prBool kPrFalse = 0;

#define imInvalidHandleValue  ((imFileRef)(LONG_PTR)-1)

#define PREMPLUGENTRY  csSDK_int32
#define DllExport      __declspec(dllexport)

struct PPix;
typedef PPix** PPixHand;

struct prSDKOpaque16 { csSDK_int64 opaque[2]; };
typedef prSDKOpaque16 PrSDKString;
typedef prSDKOpaque16 PrSDKColorSpaceID;

typedef struct { csSDK_int32 left, top, right, bottom; } prRect;

inline void prSetRect(prRect* r, int left, int top, int right, int bottom)
{
	r->left = left; r->top = top; r->right = right; r->bottom = bottom;
}

inline void prUTF16CharCopy(prUTF16Char* dst, const prUTF16Char* src)
{
	if (!dst || !src) return;
	while ((*dst++ = *src++) != 0) {}
}

enum {
	imInit                          = 0,
	imShutdown                      = 1,
	imQuietFile                     = 8,
	imCloseFile                     = 9,
	imAnalysis                      = 12,
	imDataRateAnalysis              = 13,
	imGetIndFormat                  = 14,
	imGetMetaData                   = 23,
	imSetMetaData                   = 24,
	imImportAudio7                  = 25,
	imGetIndPixelFormat             = 27,
	imGetSupports8                  = 36,
	imGetInfo8                      = 37,
	imGetPrefs8                     = 38,
	imOpenFile8                     = 39,
	imGetPreferredFrameSize         = 45,
	imCreateAsyncImporter           = 46,
	imGetSourceVideo                = 47,
	imGetTimeInfo8                  = 52,
	imPerformSourceSettingsCommand  = 66,
	imGetIndColorSpace              = 73,
};

enum {
	malNoError        = 0,
	imBadFile         = 2,
	imUnsupported     = 3,
	imMemErr          = 4,
	imOtherErr        = 5,
	imFrameNotFound   = 28,
	malSupports8      = 101,
	imBadFormatIndex  = 301,
	imIsCacheable     = 400,
};

enum {
	aiInitiateAsyncRead = 0,
	aiCancelAsyncRead   = 1,
	aiFlush             = 2,
	aiGetFrame          = 3,
	aiClose             = 4,
};
enum {
	aiNoError       = 0,
	aiUnsupported   = 1,
	aiFrameNotFound = 3,
};

const prSuiteError suiteError_NoError = 0;

#define IMPORTMOD_VERSION_6  6

const csSDK_int32 kPrAudioChannelType_Mono   = 1;
const csSDK_int32 kPrAudioChannelType_Stereo = 2;
const csSDK_int32 kPrAudioChannelType_51     = 3;

const PrAudioSampleType kPrAudioSampleType_32BitFloat = 6;

const char alphaStraight = 1;
const char alphaNone     = 5;

const csSDK_int32 imColorSpaceSupport_Fixed    = 1;
const csSDK_int32 kPrSDKColorSpaceType_SEITags = 3;

const csSDK_int32 xfIsMovie   = 0x0008;
const csSDK_int32 xfCanImport = 0x4000;
const csSDK_int32 xfCanOpen   = 0x8000;

const csSDK_int32 kRandomAccessImport = 0;

const PrVideoFrameRates kVideoFrameRate_24Drop  = 1;
const PrVideoFrameRates kVideoFrameRate_NTSC    = 4;
const PrVideoFrameRates kVideoFrameRate_NTSC_HD = 7;

const PrPPixBufferAccess PrPPixBufferAccess_ReadWrite = 2;

const PrFileOpenAccess kPrOpenFileAccess_ReadOnly  = 0;
const PrFileOpenAccess kPrOpenFileAccess_ReadWrite = 1;

const imRenderIntent imRenderIntent_Scrubbing = 2;

const PrPixelFormat PrPixelFormat_Any            = 0;
const PrPixelFormat PrPixelFormat_BGRA_4444_8u   = 0x61726762;
const PrPixelFormat PrPixelFormat_BGRA_4444_16u  = 0x61726742;
const PrPixelFormat PrPixelFormat_UYVY_422_8u_601 = 0x79767975;
const PrPixelFormat PrPixelFormat_UYVY_422_8u_709 = 0x37767975;
const PrPixelFormat PrPixelFormat_YUYV_422_8u_601 = 0x32797579;
const PrPixelFormat PrPixelFormat_YUYV_422_8u_709 = 0x33797579;
const PrPixelFormat PrPixelFormat_Invalid        = 0x66646162;

typedef struct
{
	char*       (*newPtr)(csSDK_uint32 size);
	void        (*setPtrSize)(PrMemoryPtr* ptr, csSDK_uint32 newSize);
	csSDK_int32 (*getPtrSize)(char* ptr);
	void        (*disposePtr)(char* ptr);
	char**      (*newHandle)(csSDK_uint32 size);
	csSDK_int16 (*setHandleSize)(PrMemoryHandle h, csSDK_uint32 newSize);
	csSDK_int32 (*getHandleSize)(PrMemoryHandle h);
	void        (*disposeHandle)(PrMemoryHandle h);
	char*       (*newPtrClear)(csSDK_uint32 size);
	char**      (*newHandleClear)(csSDK_uint32 size);
	void        (*lockHandle)(PrMemoryHandle h);
	void        (*unlockHandle)(PrMemoryHandle h);
} PlugMemoryFuncs, *PlugMemoryFuncsPtr;

typedef struct SPBasicSuite
{
	SPErr (*AcquireSuite)(const char* name, int version, const void** suite);
	SPErr (*ReleaseSuite)(const char* name, int version);
} SPBasicSuite;

typedef struct
{
	void*           slot0[7];
	SPBasicSuite*   (*getSPBasicSuite)();
} PlugUtilFuncs;

typedef struct
{
	int             piInterfaceVer;
	PlugMemoryFuncs* memFuncs;
	void*           windFuncs;
	void*           ppixFuncs;
	PlugUtilFuncs*  utilFuncs;
	void*           timelineFuncs;
} piSuites, *piSuitesPtr;

typedef struct
{
	csSDK_int32     imInterfaceVer;
	void*           funcs;
	piSuitesPtr     piSuites;
} imStdParms;

#define kPrSDKPPixCreatorSuite          "Premiere PPix Creator Suite"
#define kPrSDKPPixCreatorSuiteVersion   1
#define kPrSDKPPixCacheSuite            "Premiere PPix Cache Suite"
#define kPrSDKPPixCacheSuiteVersion     8
#define kPrSDKPPixSuite                 "Premiere PPix Suite"
#define kPrSDKPPixSuiteVersion          1
#define kPrSDKTimeSuite                 "Premiere Time Suite"
#define kPrSDKTimeSuiteVersion          1

typedef struct
{
	csSDK_int32     inFrameWidth;
	csSDK_int32     inFrameHeight;
	PrPixelFormat   inPixelFormat;
} imFrameFormat;

typedef struct
{
	prSuiteError (*CreatePPix)(PPixHand* outPPix, PrPPixBufferAccess access, PrPixelFormat format, const prRect* bounds);
} PrSDKPPixCreatorSuite;

typedef struct
{
	prSuiteError (*AddFrameToCache)(csSDK_uint32 importerID, csSDK_int32 stream, PPixHand ppix,
	                                csSDK_int32 frame, void* prefs, csSDK_int32 prefsLength);
	prSuiteError (*GetFrameFromCache)(csSDK_uint32 importerID, csSDK_int32 stream, csSDK_int32 frame,
	                                  csSDK_int32 numFormats, imFrameFormat* formats, PPixHand* outPPix,
	                                  void* prefs, csSDK_int32 prefsLength);
} PrSDKPPixCacheSuite;

typedef struct
{
	prSuiteError (*Dispose)(PPixHand ppix);
	prSuiteError (*GetPixels)(PPixHand ppix, PrPPixBufferAccess access, char** outPixels);
	void*        GetBounds;
	prSuiteError (*GetRowBytes)(PPixHand ppix, csSDK_int32* outRowBytes);
} PrSDKPPixSuite;

typedef struct
{
	prSuiteError (*GetTicksPerSecond)(PrTime* outTicks);
	prSuiteError (*GetTicksPerVideoFrame)(PrVideoFrameRates rate, PrTime* outTicks);
} PrSDKTimeSuite;

typedef struct
{
	csSDK_uint32    importerType;
	csSDK_int32     unused1;
	csSDK_int32     canSave;
	csSDK_int32     canDelete;
	csSDK_int32     canResize;
	csSDK_int32     canDoSubsize;
	csSDK_int32     canDoContinuousTime;
	csSDK_int32     noFile;
	csSDK_int32     addToMenu;
	csSDK_int32     hasSetup;
	csSDK_int32     dontCache;
	csSDK_int32     setupOnDblClk;
	csSDK_int32     keepLoaded;
	csSDK_int32     priority;
	csSDK_int32     canAsync;
	csSDK_int32     canCreate;
	csSDK_int32     canCalcSizes;
	csSDK_int32     canTrim;
	csSDK_int32     avoidAudioConform;
	prUTF16Char*    acceleratorFileExt;
	csSDK_int32     canCopy;
	csSDK_int32     canSupplyMetadataClipName;
	csSDK_int32     canValidatePrefs;
	csSDK_int32     canProvidePeakAudio;
	csSDK_int32     canProvideFileList;
	csSDK_int32     canProvideClosedCaptions;
	char            fileInfoVersion[37];
	csSDK_int32     hasSourceSettingsEffect;
} imImportInfoRec;

typedef struct
{
	csSDK_int32     filetype;
	csSDK_int32     flags;
	csSDK_int32     canWriteTimecode;
	char            FormatName[256];
	char            FormatShortName[32];
	char            PlatformExtension[256];
	prBool          hasAlternateTypes;
	csSDK_int32     alternateTypes[50];
	csSDK_int32     canWriteMetaData;
} imIndFormatRec;

typedef struct
{
	void*               importID;
	csSDK_int32         filetype;
	const prUTF16Char*  filepath;
	imFileRef           fileref;
	PrMemoryPtr         newfilename;
} imFileAccessRec8;

typedef struct
{
	imFileAccessRec8    fileinfo;
	void*               privatedata;
	csSDK_int32         delegatetype;
	PrFileOpenAccess    inReadWrite;
	csSDK_int32         inImporterID;
	csSDK_size_t        outExtraMemoryUsage;
	csSDK_int32         inStreamIdx;
} imFileOpenRec8;

typedef struct
{
	csSDK_int32     imageWidth;
	csSDK_int32     imageHeight;
	csSDK_uint16    pixelAspectV1;
	csSDK_int16     depth;
	csSDK_int32     subType;
	char            fieldType;
	char            fieldsStacked;
	char            reserved_1;
	char            reserved_2;
	char            alphaType;
	unsigned char   matteColor[3];
	char            alphaInverted;
	char            isVectors;
	char            drawsExternal;
	char            canForceInternalDraw;
	char            dontObscure;
	char            isStill;
	char            noDuration;
	char            reserved_3;
	csSDK_uint32    pixelAspectNum;
	csSDK_uint32    pixelAspectDen;
	char            isRollCrawl;
	char            reservedc[3];
	csSDK_int32     importerID;
	csSDK_int32     supportsAsyncIO;
	csSDK_int32     supportsGetSourceVideo;
	csSDK_int32     hasPulldown;
	csSDK_int32     pulldownCadence;
	csSDK_int32     posterFrame;
	csSDK_int32     canTransform;
	csSDK_int32     interpretationUncertain;
	csSDK_int32     colorProfileSupport;
	PrSDKString     codecDescription;
	csSDK_int32     colorSpaceSupport;
	PrTime          frameRate;
	prBool          hasEmbeddedLUT;
	csSDK_int32     bitDepth;
	csSDK_int32     reserved[11];
} imImageInfoRec;

typedef struct
{
	csSDK_int32         numChannels;
	float               sampleRate;
	PrAudioSampleType   sampleType;
} imAudioInfoRec7;

typedef struct
{
	char            hasVideo;
	char            hasAudio;
	imImageInfoRec  vidInfo;
	csSDK_int32     vidScale;
	csSDK_int32     vidSampleSize;
	csSDK_int32     vidDuration;
	imAudioInfoRec7 audInfo;
	PrAudioSample   audDuration;
	csSDK_int32     accessModes;
	void*           privatedata;
	void*           prefs;
	char            hasDataRate;
} imFileInfoRec8;

typedef struct
{
	void*           privatedata;
	PrPixelFormat   outPixelFormat;
	const void*     prefs;
} imIndPixelFormatRec;

typedef struct
{
	csSDK_int32     colorPrimariesCode;
	csSDK_int32     transferCharacteristicCode;
	csSDK_int32     matrixEquationsCode;
	csSDK_int32     bitDepth;
	prBool          isFullRange;
	prBool          isRGB;
	prBool          isSceneReferred;
} prSEIColorCodesRec;

#pragma pack(push, 8)
typedef struct
{
	void*               inPrivateData;
	csSDK_int32         outColorSpaceType;
	struct {
		csSDK_int32     ioBufferSize;
		void*           inDestinationBuffer;
		PrSDKString     outName;
	}                   ioProfileRec;
	prSEIColorCodesRec  outSEICodesRec;
} imIndColorSpaceRec;
#pragma pack(pop)

typedef struct
{
	void*           privatedata;
	void*           prefs;
	char            orgtime[18];
	csSDK_int32     orgScale;
	csSDK_int32     orgSampleSize;
	char            alttime[18];
	csSDK_int32     altScale;
	csSDK_int32     altSampleSize;
	char            orgreel[40];
	char            altreel[40];
	char            logcomment[256];
	csSDK_int32     dataType;
} imTimeInfoRec8;

typedef struct
{
	void*           privatedata;
	void*           prefs;
	csSDK_int32     buffersize;
	char*           buffer;
	csSDK_int32     timecodeFormat;
} imAnalysisRec;

typedef struct
{
	void*           privatedata;
	void*           prefs;
	csSDK_int32     buffersize;
	char*           buffer;
	csSDK_int32     baserate;
} imDataRateAnalysisRec;

typedef struct
{
	csSDK_uint32    sampledur;
	csSDK_uint32    samplesize;
} imDataSample;

typedef struct
{
	PrAudioSample   position;
	csSDK_uint32    size;
	float**         buffer;
	void*           privateData;
	void*           prefs;
} imImportAudioRec7;

typedef struct
{
	char*           prefs;
	csSDK_int32     prefsLength;
} imGetPrefsRec;

typedef struct
{
	void*           inPrivateData;
	void*           inPrefs;
	PrPixelFormat   inPixelFormat;
	csSDK_int32     inIndex;
	csSDK_int32     outWidth;
	csSDK_int32     outHeight;
} imPreferredFrameSizeRec;

typedef struct
{
	imRenderIntent  inIntent;
	double          inPlaybackRatio;
	double          inPlaybackRate;
} imRenderContext;

typedef struct
{
	void*               inPrivateData;
	csSDK_int32         currentStreamIdx;
	PrTime              inFrameTime;
	imFrameFormat*      inFrameFormats;
	csSDK_int32         inNumFrameFormats;
	bool                removePulldown;
	char                unused2[3];
	PPixHand*           outFrame;
	void*               prefs;
	csSDK_int32         prefsSize;
	PrSDKString         selectedColorProfileName;
	PrRenderQuality     inQuality;
	imRenderContext     inRenderContext;
	PrSDKColorSpaceID   opaqueColorSpaceIdentifier;
} imSourceVideoRec;

typedef PREMPLUGENTRY (*AsyncImporterEntry)(int selector, void* param);

typedef struct
{
	void*               inPrivateData;
	void*               inPrefs;
	AsyncImporterEntry  outAsyncEntry;
	void*               outAsyncPrivateData;
} imAsyncImporterCreationRec;

typedef struct
{
	void*               inPrivateData;
	long                unused1;
	imSourceVideoRec    inSourceRec;
} aiAsyncRequest;

#pragma pack(pop)

#endif
