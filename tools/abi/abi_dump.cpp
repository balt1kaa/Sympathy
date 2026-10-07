#include <cstdio>
#include <cstddef>
#include <cstdint>

#ifdef WITH_ADOBE_SDK
#include <windows.h>
#include "PrSDKImport.h"
#include "PrSDKAsyncImporter.h"
#include "PrSDKPPixCreatorSuite.h"
#include "PrSDKPPixCacheSuite.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKTimeSuite.h"
#include "PrSDKMALErrors.h"
#include "SPBasic.h"
#else
#include "../../src/premiere_api.h"
#endif

#define SIZE(t)     printf("size   %-28s %zu\n", #t, sizeof(t));
#define AT(t, m)    printf("offset %-28s %-28s %zu\n", #t, #m, offsetof(t, m));
#define VAL(x)      printf("value  %-40s %lld\n", #x, (long long)(x));
#define STR(x)      printf("string %-40s %s\n", #x, x);

int main()
{
	SIZE(prRect) AT(prRect, left) AT(prRect, top) AT(prRect, right) AT(prRect, bottom)

	SIZE(PlugMemoryFuncs)
	AT(PlugMemoryFuncs, newPtr) AT(PlugMemoryFuncs, setPtrSize) AT(PlugMemoryFuncs, getPtrSize) AT(PlugMemoryFuncs, disposePtr)
	AT(PlugMemoryFuncs, newHandle) AT(PlugMemoryFuncs, setHandleSize) AT(PlugMemoryFuncs, getHandleSize)
	AT(PlugMemoryFuncs, disposeHandle) AT(PlugMemoryFuncs, newPtrClear) AT(PlugMemoryFuncs, newHandleClear)
	AT(PlugMemoryFuncs, lockHandle) AT(PlugMemoryFuncs, unlockHandle)
	AT(SPBasicSuite, AcquireSuite) AT(SPBasicSuite, ReleaseSuite)
	AT(PlugUtilFuncs, getSPBasicSuite)
	SIZE(piSuites)
	AT(piSuites, piInterfaceVer) AT(piSuites, memFuncs) AT(piSuites, windFuncs) AT(piSuites, ppixFuncs)
	AT(piSuites, utilFuncs) AT(piSuites, timelineFuncs)
	SIZE(imStdParms) AT(imStdParms, imInterfaceVer) AT(imStdParms, funcs) AT(imStdParms, piSuites)

	SIZE(imFrameFormat) AT(imFrameFormat, inFrameWidth) AT(imFrameFormat, inFrameHeight) AT(imFrameFormat, inPixelFormat)
	AT(PrSDKPPixCreatorSuite, CreatePPix)
	AT(PrSDKPPixCacheSuite, AddFrameToCache) AT(PrSDKPPixCacheSuite, GetFrameFromCache)
	AT(PrSDKPPixSuite, Dispose) AT(PrSDKPPixSuite, GetPixels) AT(PrSDKPPixSuite, GetBounds) AT(PrSDKPPixSuite, GetRowBytes)
	AT(PrSDKTimeSuite, GetTicksPerSecond) AT(PrSDKTimeSuite, GetTicksPerVideoFrame)

	AT(imImportInfoRec, importerType) AT(imImportInfoRec, unused1) AT(imImportInfoRec, canSave) AT(imImportInfoRec, canDelete)
	AT(imImportInfoRec, canResize) AT(imImportInfoRec, canDoSubsize) AT(imImportInfoRec, canDoContinuousTime)
	AT(imImportInfoRec, noFile) AT(imImportInfoRec, addToMenu) AT(imImportInfoRec, hasSetup) AT(imImportInfoRec, dontCache)
	AT(imImportInfoRec, setupOnDblClk) AT(imImportInfoRec, keepLoaded) AT(imImportInfoRec, priority) AT(imImportInfoRec, canAsync)
	AT(imImportInfoRec, canCreate) AT(imImportInfoRec, canCalcSizes) AT(imImportInfoRec, canTrim)
	AT(imImportInfoRec, avoidAudioConform) AT(imImportInfoRec, acceleratorFileExt) AT(imImportInfoRec, canCopy)
	AT(imImportInfoRec, canSupplyMetadataClipName) AT(imImportInfoRec, canValidatePrefs) AT(imImportInfoRec, canProvidePeakAudio)
	AT(imImportInfoRec, canProvideFileList) AT(imImportInfoRec, canProvideClosedCaptions) AT(imImportInfoRec, fileInfoVersion)
	AT(imImportInfoRec, hasSourceSettingsEffect)

	SIZE(imIndFormatRec)
	AT(imIndFormatRec, filetype) AT(imIndFormatRec, flags) AT(imIndFormatRec, canWriteTimecode) AT(imIndFormatRec, FormatName)
	AT(imIndFormatRec, FormatShortName) AT(imIndFormatRec, PlatformExtension) AT(imIndFormatRec, hasAlternateTypes)
	AT(imIndFormatRec, alternateTypes) AT(imIndFormatRec, canWriteMetaData)

	SIZE(imFileAccessRec8)
	AT(imFileAccessRec8, importID) AT(imFileAccessRec8, filetype) AT(imFileAccessRec8, filepath) AT(imFileAccessRec8, fileref)
	AT(imFileAccessRec8, newfilename)
	SIZE(imFileOpenRec8)
	AT(imFileOpenRec8, fileinfo) AT(imFileOpenRec8, privatedata) AT(imFileOpenRec8, delegatetype) AT(imFileOpenRec8, inReadWrite)
	AT(imFileOpenRec8, inImporterID) AT(imFileOpenRec8, outExtraMemoryUsage) AT(imFileOpenRec8, inStreamIdx)

	SIZE(imImageInfoRec)
	AT(imImageInfoRec, imageWidth) AT(imImageInfoRec, imageHeight) AT(imImageInfoRec, pixelAspectV1) AT(imImageInfoRec, depth)
	AT(imImageInfoRec, subType) AT(imImageInfoRec, fieldType) AT(imImageInfoRec, fieldsStacked) AT(imImageInfoRec, reserved_1)
	AT(imImageInfoRec, reserved_2) AT(imImageInfoRec, alphaType) AT(imImageInfoRec, matteColor) AT(imImageInfoRec, alphaInverted)
	AT(imImageInfoRec, isVectors) AT(imImageInfoRec, drawsExternal) AT(imImageInfoRec, canForceInternalDraw)
	AT(imImageInfoRec, dontObscure) AT(imImageInfoRec, isStill) AT(imImageInfoRec, noDuration) AT(imImageInfoRec, reserved_3)
	AT(imImageInfoRec, pixelAspectNum) AT(imImageInfoRec, pixelAspectDen) AT(imImageInfoRec, isRollCrawl)
	AT(imImageInfoRec, reservedc) AT(imImageInfoRec, importerID) AT(imImageInfoRec, supportsAsyncIO)
	AT(imImageInfoRec, supportsGetSourceVideo) AT(imImageInfoRec, hasPulldown) AT(imImageInfoRec, pulldownCadence)
	AT(imImageInfoRec, posterFrame) AT(imImageInfoRec, canTransform) AT(imImageInfoRec, interpretationUncertain)
	AT(imImageInfoRec, colorProfileSupport) AT(imImageInfoRec, codecDescription) AT(imImageInfoRec, colorSpaceSupport)
	AT(imImageInfoRec, frameRate) AT(imImageInfoRec, hasEmbeddedLUT) AT(imImageInfoRec, bitDepth) AT(imImageInfoRec, reserved)
	SIZE(imAudioInfoRec7) AT(imAudioInfoRec7, numChannels) AT(imAudioInfoRec7, sampleRate) AT(imAudioInfoRec7, sampleType)

	AT(imFileInfoRec8, hasVideo) AT(imFileInfoRec8, hasAudio) AT(imFileInfoRec8, vidInfo) AT(imFileInfoRec8, vidScale)
	AT(imFileInfoRec8, vidSampleSize) AT(imFileInfoRec8, vidDuration) AT(imFileInfoRec8, audInfo) AT(imFileInfoRec8, audDuration)
	AT(imFileInfoRec8, accessModes) AT(imFileInfoRec8, privatedata) AT(imFileInfoRec8, prefs) AT(imFileInfoRec8, hasDataRate)

	SIZE(imIndPixelFormatRec) AT(imIndPixelFormatRec, privatedata) AT(imIndPixelFormatRec, outPixelFormat) AT(imIndPixelFormatRec, prefs)
	SIZE(prSEIColorCodesRec)
	AT(prSEIColorCodesRec, colorPrimariesCode) AT(prSEIColorCodesRec, transferCharacteristicCode)
	AT(prSEIColorCodesRec, matrixEquationsCode) AT(prSEIColorCodesRec, bitDepth) AT(prSEIColorCodesRec, isFullRange)
	AT(prSEIColorCodesRec, isRGB) AT(prSEIColorCodesRec, isSceneReferred)
	SIZE(imIndColorSpaceRec)
	AT(imIndColorSpaceRec, inPrivateData) AT(imIndColorSpaceRec, outColorSpaceType) AT(imIndColorSpaceRec, ioProfileRec)
	AT(imIndColorSpaceRec, outSEICodesRec)

	SIZE(imTimeInfoRec8)
	AT(imTimeInfoRec8, privatedata) AT(imTimeInfoRec8, prefs) AT(imTimeInfoRec8, orgtime) AT(imTimeInfoRec8, orgScale)
	AT(imTimeInfoRec8, orgSampleSize) AT(imTimeInfoRec8, alttime) AT(imTimeInfoRec8, altScale) AT(imTimeInfoRec8, altSampleSize)
	AT(imTimeInfoRec8, orgreel) AT(imTimeInfoRec8, altreel) AT(imTimeInfoRec8, logcomment) AT(imTimeInfoRec8, dataType)
	SIZE(imAnalysisRec)
	AT(imAnalysisRec, privatedata) AT(imAnalysisRec, prefs) AT(imAnalysisRec, buffersize) AT(imAnalysisRec, buffer)
	AT(imAnalysisRec, timecodeFormat)
	SIZE(imDataRateAnalysisRec)
	AT(imDataRateAnalysisRec, privatedata) AT(imDataRateAnalysisRec, prefs) AT(imDataRateAnalysisRec, buffersize)
	AT(imDataRateAnalysisRec, buffer) AT(imDataRateAnalysisRec, baserate)
	SIZE(imDataSample) AT(imDataSample, sampledur) AT(imDataSample, samplesize)
	SIZE(imImportAudioRec7)
	AT(imImportAudioRec7, position) AT(imImportAudioRec7, size) AT(imImportAudioRec7, buffer) AT(imImportAudioRec7, privateData)
	AT(imImportAudioRec7, prefs)
	AT(imGetPrefsRec, prefs) AT(imGetPrefsRec, prefsLength)
	SIZE(imPreferredFrameSizeRec)
	AT(imPreferredFrameSizeRec, inPrivateData) AT(imPreferredFrameSizeRec, inPrefs) AT(imPreferredFrameSizeRec, inPixelFormat)
	AT(imPreferredFrameSizeRec, inIndex) AT(imPreferredFrameSizeRec, outWidth) AT(imPreferredFrameSizeRec, outHeight)
	SIZE(imRenderContext) AT(imRenderContext, inIntent) AT(imRenderContext, inPlaybackRatio) AT(imRenderContext, inPlaybackRate)
	SIZE(imSourceVideoRec)
	AT(imSourceVideoRec, inPrivateData) AT(imSourceVideoRec, currentStreamIdx) AT(imSourceVideoRec, inFrameTime)
	AT(imSourceVideoRec, inFrameFormats) AT(imSourceVideoRec, inNumFrameFormats) AT(imSourceVideoRec, removePulldown)
	AT(imSourceVideoRec, unused2) AT(imSourceVideoRec, outFrame) AT(imSourceVideoRec, prefs) AT(imSourceVideoRec, prefsSize)
	AT(imSourceVideoRec, selectedColorProfileName) AT(imSourceVideoRec, inQuality) AT(imSourceVideoRec, inRenderContext)
	AT(imSourceVideoRec, opaqueColorSpaceIdentifier)
	SIZE(imAsyncImporterCreationRec)
	AT(imAsyncImporterCreationRec, inPrivateData) AT(imAsyncImporterCreationRec, inPrefs)
	AT(imAsyncImporterCreationRec, outAsyncEntry) AT(imAsyncImporterCreationRec, outAsyncPrivateData)
	SIZE(aiAsyncRequest) AT(aiAsyncRequest, inPrivateData) AT(aiAsyncRequest, unused1) AT(aiAsyncRequest, inSourceRec)

	SIZE(PrPixelFormat) SIZE(PrTime) SIZE(PrAudioSample) SIZE(prUTF16Char) SIZE(imFileRef) SIZE(PPixHand) SIZE(csSDK_size_t)
	SIZE(PrRenderQuality) SIZE(imRenderIntent) SIZE(PrFileOpenAccess) SIZE(PrAudioSampleType) SIZE(PrVideoFrameRates)
	SIZE(PrPPixBufferAccess) SIZE(prBool) SIZE(prMALError) SIZE(prSuiteError)

	VAL(imInit) VAL(imShutdown) VAL(imGetPrefs8) VAL(imGetInfo8) VAL(imImportAudio7) VAL(imOpenFile8) VAL(imQuietFile)
	VAL(imCloseFile) VAL(imGetTimeInfo8) VAL(imAnalysis) VAL(imDataRateAnalysis) VAL(imGetIndFormat) VAL(imGetMetaData)
	VAL(imSetMetaData) VAL(imGetIndColorSpace) VAL(imGetIndPixelFormat) VAL(imGetSupports8) VAL(imGetPreferredFrameSize)
	VAL(imGetSourceVideo) VAL(imCreateAsyncImporter) VAL(imPerformSourceSettingsCommand)
	VAL(malNoError) VAL(malSupports8) VAL(imUnsupported) VAL(imBadFile) VAL(imBadFormatIndex) VAL(imOtherErr) VAL(imMemErr)
	VAL(imIsCacheable) VAL(imFrameNotFound)
	VAL(aiInitiateAsyncRead) VAL(aiCancelAsyncRead) VAL(aiFlush) VAL(aiGetFrame) VAL(aiClose)
	VAL(aiNoError) VAL(aiUnsupported) VAL(aiFrameNotFound)
	VAL(IMPORTMOD_VERSION_6) VAL(kPrTrue) VAL(kPrFalse) VAL(suiteError_NoError)
	VAL(kPrAudioChannelType_Mono) VAL(kPrAudioChannelType_Stereo) VAL(kPrAudioChannelType_51) VAL(kPrAudioSampleType_32BitFloat)
	VAL(alphaNone) VAL(alphaStraight) VAL(imColorSpaceSupport_Fixed) VAL(kPrSDKColorSpaceType_SEITags)
	VAL(xfCanOpen) VAL(xfCanImport) VAL(xfIsMovie) VAL(kRandomAccessImport)
	VAL(kVideoFrameRate_24Drop) VAL(kVideoFrameRate_NTSC) VAL(kVideoFrameRate_NTSC_HD)
	VAL(PrPPixBufferAccess_ReadWrite) VAL(kPrOpenFileAccess_ReadOnly) VAL(kPrOpenFileAccess_ReadWrite) VAL(imRenderIntent_Scrubbing)
	VAL(PrPixelFormat_Any) VAL(PrPixelFormat_BGRA_4444_8u) VAL(PrPixelFormat_BGRA_4444_16u) VAL(PrPixelFormat_Invalid)
	VAL(PrPixelFormat_UYVY_422_8u_601) VAL(PrPixelFormat_UYVY_422_8u_709) VAL(PrPixelFormat_YUYV_422_8u_601)
	VAL(PrPixelFormat_YUYV_422_8u_709)
	VAL((long long)(intptr_t)imInvalidHandleValue)
	VAL(kPrSDKPPixCreatorSuiteVersion) VAL(kPrSDKPPixCacheSuiteVersion) VAL(kPrSDKPPixSuiteVersion) VAL(kPrSDKTimeSuiteVersion)
	STR(kPrSDKPPixCreatorSuite) STR(kPrSDKPPixCacheSuite) STR(kPrSDKPPixSuite) STR(kPrSDKTimeSuite)
	return 0;
}
