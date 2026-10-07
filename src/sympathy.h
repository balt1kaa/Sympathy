#ifndef SYMPATHY_H
#define SYMPATHY_H

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "premiere_api.h"

#define		IMPORTER_NAME			"sympathy"

struct AshenClip
{
	prBool			hasVideo;
	prBool			hasAudio;
	PrFourCC		videoSubtype;
	PrPixelFormat	pixelFormat;
	csSDK_int32		depth;
	csSDK_int32		width;
	csSDK_int32		height;
	csSDK_uint32	numFrames;
	PrTime			frameRate;
	csSDK_int32		fieldType;
	csSDK_uint32	pixelAspectNum;
	csSDK_uint32	pixelAspectDen;
	PrAudioSample	numSampleFrames;
	csSDK_int32		channelType;
	double			sampleRate;
	csSDK_int32		hasAlpha;
	csSDK_int32		colorPrimaries;
	csSDK_int32		colorTrc;
	csSDK_int32		colorMatrix;
	csSDK_int32		mediaBitDepth;
};

struct AshenLocal
{
	AshenClip				clip;
	PrAudioSample			audioPosition;
	prUTF16Char				fileName[256];
	imFileRef				fileRef;
	PlugMemoryFuncsPtr		memFuncs;
	csSDK_int32				importerID;
	SPBasicSuite			*BasicSuite;
	PrSDKPPixCreatorSuite	*PPixCreatorSuite;
	PrSDKPPixCacheSuite		*PPixCacheSuite;
	PrSDKPPixSuite			*PPixSuite;
	PrSDKTimeSuite			*TimeSuite;
};
typedef AshenLocal** AshenLocalH;

inline int Ashen_ChannelCount(csSDK_int32 channelType)
{
	switch (channelType) {
		case kPrAudioChannelType_Mono:   return 1;
		case kPrAudioChannelType_Stereo: return 2;
		case kPrAudioChannelType_51:     return 6;
		default:                         return -1;
	}
}

extern "C" {
PREMPLUGENTRY DllExport xImportEntry (csSDK_int32	selector,
									  imStdParms	*stdParms,
									  void			*param1,
									  void			*param2);
}

prMALError AshenDecodeToBuffer(int importerID, const prUTF16Char* path16,
							   csSDK_int32 frameIndex, int width, int height,
							   int dstStride, char* dst, int lowresHint,
							   PrPixelFormat outFmt);

int AshenLowresHintFor(int intent, double playbackRatio);

void AshenGetLog(int importerID, const char* kind, int frame, double waitMs, int queue,
				 int intent, int width, int height, int pixfmt);
double AshenNowMs(void);

int Ashen_CanDeliver(PrPixelFormat f);

void AshenCacheAddLog(int importerID, int frame, int rc);

void AshenFlushLog(int importerID, int dropped, int inProgress, double ms);

void AshenDupFrameLog(int importerID, int frame, int waiters);

void AshenUndeliverableFormatLog(int wanted, int preferred, int nOffered);

void AshenBufferFailLog(int importerID, int frame, int w, int h, const char* stage);

void AshenHostFormatsLog(int count, const char* list);

#endif
