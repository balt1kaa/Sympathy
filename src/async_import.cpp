#include "async_import.h"
#include <cstring>

PREMPLUGENTRY xAsyncImportEntry(int inSelector, void *inParam)
{
	csSDK_int32			result			= aiUnsupported;
	AshenAsyncImporter	*asyncImporter	= 0;

	switch (inSelector)
	{
		case aiInitiateAsyncRead:
		{
			aiAsyncRequest* req = reinterpret_cast<aiAsyncRequest*>(inParam);
			asyncImporter = reinterpret_cast<AshenAsyncImporter*>(req->inPrivateData);
			result = asyncImporter->OnInitiateAsyncRead(req->inSourceRec);
			break;
		}
		case aiCancelAsyncRead:
		{
			aiAsyncRequest* req = reinterpret_cast<aiAsyncRequest*>(inParam);
			asyncImporter = reinterpret_cast<AshenAsyncImporter*>(req->inPrivateData);
			result = asyncImporter->OnCancelAsyncRead(req->inSourceRec);
			break;
		}
		case aiFlush:
			asyncImporter = reinterpret_cast<AshenAsyncImporter*>(inParam);
			result = asyncImporter->OnFlush();
			break;
		case aiGetFrame:
		{
			imSourceVideoRec* rec = reinterpret_cast<imSourceVideoRec*>(inParam);
			asyncImporter = reinterpret_cast<AshenAsyncImporter*>(rec->inPrivateData);
			result = asyncImporter->OnGetFrame(rec);
			break;
		}
		case aiClose:
			asyncImporter = reinterpret_cast<AshenAsyncImporter*>(inParam);
			delete asyncImporter;
			result = aiNoError;
			break;
	}
	return result;
}

AshenAsyncImporter::AshenAsyncImporter(imStdParms *stdParms, AshenLocalH inRecH)
{
	stop = false;

	localRecH = (AshenLocalH) stdParms->piSuites->memFuncs->newHandle(sizeof(AshenLocal));
	memcpy((*localRecH), (*inRecH), sizeof(AshenLocal));

	SPBasicSuite* bs = (*localRecH)->BasicSuite;
	if (bs)
	{
		bs->AcquireSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion, (const void**)&(*localRecH)->PPixCreatorSuite);
		bs->AcquireSuite(kPrSDKPPixCacheSuite,   kPrSDKPPixCacheSuiteVersion,   (const void**)&(*localRecH)->PPixCacheSuite);
		bs->AcquireSuite(kPrSDKPPixSuite,        kPrSDKPPixSuiteVersion,        (const void**)&(*localRecH)->PPixSuite);
		bs->AcquireSuite(kPrSDKTimeSuite,        kPrSDKTimeSuiteVersion,        (const void**)&(*localRecH)->TimeSuite);
	}

	const int nWorkers = 4;
	for (int i = 0; i < nWorkers; ++i)
		workers.emplace_back(&AshenAsyncImporter::WorkerLoop, this);
}

AshenAsyncImporter::~AshenAsyncImporter()
{
	{
		std::lock_guard<std::mutex> lk(mtx);
		stop = true;
	}
	cvWork.notify_all();
	for (std::thread& w : workers)
		if (w.joinable()) w.join();

	for (AsyncJob* j : jobs)
	{
		if (j->ppix) (*localRecH)->PPixSuite->Dispose(j->ppix);
		delete j;
	}
	jobs.clear();

	SPBasicSuite* bs = (*localRecH)->BasicSuite;
	if (bs)
	{
		bs->ReleaseSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion);
		bs->ReleaseSuite(kPrSDKPPixCacheSuite,   kPrSDKPPixCacheSuiteVersion);
		bs->ReleaseSuite(kPrSDKPPixSuite,        kPrSDKPPixSuiteVersion);
		bs->ReleaseSuite(kPrSDKTimeSuite,        kPrSDKTimeSuiteVersion);
	}
	(*localRecH)->memFuncs->disposeHandle(reinterpret_cast<char**>(localRecH));
}

static void AshenPickFormat(const imSourceVideoRec& rec, int fileW, int fileH,
							PrPixelFormat preferred, imFrameFormat* out)
{
	out->inFrameWidth  = 0;
	out->inFrameHeight = 0;
	out->inPixelFormat = preferred;

	if (rec.inFrameFormats && rec.inNumFrameFormats > 0)
	{
		csSDK_int32 idx = -1;
		for (csSDK_int32 i = 0; i < rec.inNumFrameFormats && idx < 0; ++i)
			if (rec.inFrameFormats[i].inPixelFormat == preferred) idx = i;
		for (csSDK_int32 i = 0; i < rec.inNumFrameFormats && idx < 0; ++i)
		{
			PrPixelFormat pf = rec.inFrameFormats[i].inPixelFormat;
			if (pf == PrPixelFormat_BGRA_4444_8u || pf == PrPixelFormat_Any) { idx = i; break; }
		}
		if (idx < 0) idx = 0;
		out->inPixelFormat = (rec.inFrameFormats[idx].inPixelFormat == PrPixelFormat_Any)
							 ? preferred : rec.inFrameFormats[idx].inPixelFormat;
		if (Ashen_CanDeliver(out->inPixelFormat) == 0)
		{
			AshenUndeliverableFormatLog((int)out->inPixelFormat, (int)preferred,
										(int)rec.inNumFrameFormats);
			out->inPixelFormat = PrPixelFormat_BGRA_4444_8u;
		}
		out->inFrameWidth  = rec.inFrameFormats[idx].inFrameWidth;
		out->inFrameHeight = rec.inFrameFormats[idx].inFrameHeight;
	}
	{
		static unsigned lastSig = 0;
		unsigned sig = (unsigned)rec.inNumFrameFormats * 2654435761u;
		for (csSDK_int32 i = 0; i < rec.inNumFrameFormats && rec.inFrameFormats; ++i)
			sig = sig * 16777619u ^ (unsigned)rec.inFrameFormats[i].inPixelFormat;
		if (sig != lastSig && rec.inFrameFormats && rec.inNumFrameFormats > 0) {
			lastSig = sig;
			char list[512]; int n = 0;
			for (csSDK_int32 i = 0; i < rec.inNumFrameFormats && n < 460; ++i) {
				unsigned f = (unsigned)rec.inFrameFormats[i].inPixelFormat;
				char cc[5] = { (char)(f & 0xff), (char)((f >> 8) & 0xff),
				               (char)((f >> 16) & 0xff), (char)((f >> 24) & 0xff), 0 };
				for (int k = 0; k < 4; ++k) if (cc[k] < 32 || cc[k] > 126) cc[k] = '?';
				n += _snprintf_s(list + n, sizeof(list) - n, _TRUNCATE, "%s%s@%dx%d",
				                 n ? " " : "", cc,
				                 (int)rec.inFrameFormats[i].inFrameWidth,
				                 (int)rec.inFrameFormats[i].inFrameHeight);
			}
			AshenHostFormatsLog((int)rec.inNumFrameFormats, list);
		}
	}

	if (out->inFrameWidth  <= 0) out->inFrameWidth  = fileW;
	if (out->inFrameHeight <= 0) out->inFrameHeight = fileH;

	if (fileW > 0 && out->inFrameWidth  > fileW) out->inFrameWidth  = fileW;
	if (fileH > 0 && out->inFrameHeight > fileH) out->inFrameHeight = fileH;
}

csSDK_int32 AshenAsyncImporter::FrameOf(const imSourceVideoRec& rec) const
{
	PrTime rate = (*localRecH)->clip.frameRate;
	if (rate <= 0) return 0;
	return static_cast<csSDK_int32>(rec.inFrameTime / rate);
}

void AshenAsyncImporter::WorkerLoop()
{
	for (;;)
	{
		AsyncJob* job = nullptr;
		{
			std::unique_lock<std::mutex> lk(mtx);
			cvWork.wait(lk, [&]{
				if (stop) return true;
				for (AsyncJob* j : jobs) if (j->state == kAsyncPending) return true;
				return false;
			});
			if (stop) return;
			for (AsyncJob* j : jobs) if (j->state == kAsyncPending) { job = j; break; }
			if (!job) continue;
			job->state = kAsyncInProgress;
		}

		prMALError r = AshenDecodeToBuffer((*localRecH)->importerID, (*localRecH)->fileName,
										   job->frameNum, job->width, job->height,
										   job->rowBytes, job->buffer, job->lowresHint,
										   job->pixelFormat);

		{
			std::lock_guard<std::mutex> lk(mtx);
			job->state = (r == malNoError) ? kAsyncDone : kAsyncError;
		}
		cvDone.notify_all();
	}
}

int AshenAsyncImporter::OnInitiateAsyncRead(imSourceVideoRec& rec)
{
	csSDK_int32 frameNum = FrameOf(rec);

	imFrameFormat lookFmt;
	AshenPickFormat(rec, (*localRecH)->clip.width, (*localRecH)->clip.height, (*localRecH)->clip.pixelFormat, &lookFmt);
	PPixHand cached;
	if ((*localRecH)->PPixCacheSuite->GetFrameFromCache((*localRecH)->importerID, 0, frameNum, 1,
			&lookFmt, &cached, NULL, NULL) == suiteError_NoError)
	{
		(*localRecH)->PPixSuite->Dispose(cached);
		AshenGetLog((*localRecH)->importerID, "cache", frameNum, -1.0, 0,
			rec.inRenderContext.inIntent, lookFmt.inFrameWidth, lookFmt.inFrameHeight,
			(int)lookFmt.inPixelFormat);
		return aiNoError;
	}

	{
		std::lock_guard<std::mutex> lk(mtx);
		for (AsyncJob* j : jobs) if (j->frameNum == frameNum) return aiNoError;
	}

	imFrameFormat fmt;
	AshenPickFormat(rec, (*localRecH)->clip.width, (*localRecH)->clip.height, (*localRecH)->clip.pixelFormat, &fmt);
	int w = fmt.inFrameWidth, h = fmt.inFrameHeight;

	prRect rect;
	prSetRect(&rect, 0, 0, w, h);
	PPixHand ppix = NULL;
	char* buf = NULL;
	csSDK_int32 rb = 0;
	(*localRecH)->PPixCreatorSuite->CreatePPix(&ppix, PrPPixBufferAccess_ReadWrite, fmt.inPixelFormat, &rect);
	if (!ppix) { AshenBufferFailLog((*localRecH)->importerID, frameNum, w, h, "createppix"); return aiFrameNotFound; }
	(*localRecH)->PPixSuite->GetPixels(ppix, PrPPixBufferAccess_ReadWrite, &buf);
	if (!buf) {
		AshenBufferFailLog((*localRecH)->importerID, frameNum, w, h, "getpixels");
		(*localRecH)->PPixSuite->Dispose(ppix);
		return aiFrameNotFound;
	}
	(*localRecH)->PPixSuite->GetRowBytes(ppix, &rb);

	AsyncJob* job = new AsyncJob{ frameNum, ppix, buf, rb, w, h, fmt.inPixelFormat, kAsyncPending,
								  AshenLowresHintFor(rec.inRenderContext.inIntent,
													 rec.inRenderContext.inPlaybackRatio),
								  0 , false  };
	int depth = 0;
	{
		std::lock_guard<std::mutex> lk(mtx);
		jobs.push_back(job);
		for (AsyncJob* j : jobs) if (j->state == kAsyncPending) ++depth;
	}
	cvWork.notify_one();
	AshenGetLog((*localRecH)->importerID, "prefetch", frameNum, -1.0, depth,
				rec.inRenderContext.inIntent, w, h, (int)fmt.inPixelFormat);
	return aiNoError;
}

int AshenAsyncImporter::OnCancelAsyncRead(imSourceVideoRec& rec)
{
	csSDK_int32 frameNum = FrameOf(rec);
	std::lock_guard<std::mutex> lk(mtx);
	for (auto it = jobs.begin(); it != jobs.end(); ++it)
	{
		if ((*it)->frameNum == frameNum && (*it)->state == kAsyncPending)
		{
			if ((*it)->waiters > 0)
			{
				(*it)->abandoned = true;
				(*it)->state     = kAsyncError;
				cvDone.notify_all();
				return aiNoError;
			}
			if ((*it)->ppix) (*localRecH)->PPixSuite->Dispose((*it)->ppix);
			delete *it;
			jobs.erase(it);
			AshenGetLog((*localRecH)->importerID, "cancel", frameNum, -1.0, (int)jobs.size(),
				rec.inRenderContext.inIntent, 0, 0, 0);
			return aiNoError;
		}
	}
	return aiNoError;
}

int AshenAsyncImporter::OnFlush()
{
	const double flushT0 = AshenNowMs();
	std::unique_lock<std::mutex> lk(mtx);

	int inProgressAtEntry = 0;
	for (AsyncJob* j : jobs) if (j->state == kAsyncInProgress) ++inProgressAtEntry;

	for (auto it = jobs.begin(); it != jobs.end(); )
	{
		if ((*it)->state == kAsyncInProgress) { ++it; continue; }
		if ((*it)->waiters > 0)
		{
			(*it)->abandoned = true;
			(*it)->state     = kAsyncError;
			++it;
			continue;
		}
		if ((*it)->ppix) (*localRecH)->PPixSuite->Dispose((*it)->ppix);
		delete *it;
		it = jobs.erase(it);
	}
	cvDone.notify_all();
	cvDone.wait(lk, [&]{
		for (AsyncJob* j : jobs) if (j->state == kAsyncInProgress) return false;
		return true;
	});
	int dropped = (int)jobs.size();
	for (auto it = jobs.begin(); it != jobs.end(); )
	{
		if ((*it)->waiters > 0) { (*it)->abandoned = true; (*it)->state = kAsyncError; ++it; continue; }
		if ((*it)->ppix) (*localRecH)->PPixSuite->Dispose((*it)->ppix);
		delete *it;
		it = jobs.erase(it);
	}
	cvDone.notify_all();
	AshenFlushLog((*localRecH)->importerID, dropped, inProgressAtEntry,
				  AshenNowMs() - flushT0);
	return aiNoError;
}

int AshenAsyncImporter::OnGetFrame(imSourceVideoRec* rec)
{
	csSDK_int32 frameNum = FrameOf(*rec);

	imFrameFormat lookFmt;
	AshenPickFormat(*rec, (*localRecH)->clip.width, (*localRecH)->clip.height, (*localRecH)->clip.pixelFormat, &lookFmt);
	PPixHand cached;
	if ((*localRecH)->PPixCacheSuite->GetFrameFromCache((*localRecH)->importerID, 0, frameNum, 1,
			&lookFmt, &cached, NULL, NULL) == suiteError_NoError)
	{
		*(rec->outFrame) = cached;
		AshenGetLog((*localRecH)->importerID, "cache", frameNum, -1.0, 0,
			rec->inRenderContext.inIntent, lookFmt.inFrameWidth, lookFmt.inFrameHeight,
			(int)lookFmt.inPixelFormat);
		return aiNoError;
	}

	std::unique_lock<std::mutex> lk(mtx);
	AsyncJob* job = nullptr;
	for (AsyncJob* j : jobs) if (j->frameNum == frameNum) { job = j; break; }

	if (job && job->waiters > 0)
	{
		AshenDupFrameLog((*localRecH)->importerID, frameNum, job->waiters);
		job = nullptr;
	}

	if (job)
	{
		bool wasReady = (job->state == kAsyncDone);
		int  pending  = 0;
		for (AsyncJob* j : jobs) if (j->state == kAsyncPending) ++pending;
		if (job->state == kAsyncPending)
		{
			jobs.remove(job);
			jobs.push_front(job);
			cvWork.notify_one();
		}
		double waitT0 = AshenNowMs();
		job->waiters++;
		cvDone.wait(lk, [&]{ return job->state == kAsyncDone || job->state == kAsyncError; });
		job->waiters--;
		double waited = AshenNowMs() - waitT0;
		AshenGetLog((*localRecH)->importerID, wasReady ? "ready" : "wait", frameNum,
					wasReady ? -1.0 : waited, pending,
					rec->inRenderContext.inIntent, job->width, job->height, (int)job->pixelFormat);

		int st = job->state;
		if (job->abandoned) st = kAsyncError;
		PPixHand ppix = job->ppix;
		jobs.remove(job);
		lk.unlock();
		delete job;

		if (st == kAsyncDone)
		{
			prSuiteError addRc = (*localRecH)->PPixCacheSuite->AddFrameToCache(
					(*localRecH)->importerID, 0, ppix, frameNum, NULL, NULL);
			AshenCacheAddLog((*localRecH)->importerID, frameNum, (int)addRc);
			*(rec->outFrame) = ppix;
			return aiNoError;
		}
		(*localRecH)->PPixSuite->Dispose(ppix);
		*(rec->outFrame) = NULL;
		return aiFrameNotFound;
	}
	lk.unlock();

	double syncT0 = AshenNowMs();
	imFrameFormat fmt;
	AshenPickFormat(*rec, (*localRecH)->clip.width, (*localRecH)->clip.height, (*localRecH)->clip.pixelFormat, &fmt);
	int w = fmt.inFrameWidth, h = fmt.inFrameHeight;
	prRect rect;
	prSetRect(&rect, 0, 0, w, h);
	PPixHand ppix = NULL;
	char* buf = NULL;
	csSDK_int32 rb = 0;
	(*localRecH)->PPixCreatorSuite->CreatePPix(&ppix, PrPPixBufferAccess_ReadWrite, fmt.inPixelFormat, &rect);
	if (!ppix) {
		AshenBufferFailLog((*localRecH)->importerID, frameNum, w, h, "createppix");
		*(rec->outFrame) = NULL;
		return aiFrameNotFound;
	}
	(*localRecH)->PPixSuite->GetPixels(ppix, PrPPixBufferAccess_ReadWrite, &buf);
	if (!buf) {
		AshenBufferFailLog((*localRecH)->importerID, frameNum, w, h, "getpixels");
		(*localRecH)->PPixSuite->Dispose(ppix);
		*(rec->outFrame) = NULL;
		return aiFrameNotFound;
	}
	(*localRecH)->PPixSuite->GetRowBytes(ppix, &rb);

	prMALError r = AshenDecodeToBuffer((*localRecH)->importerID, (*localRecH)->fileName,
									   frameNum, w, h, rb, buf,
									   AshenLowresHintFor(rec->inRenderContext.inIntent,
														  rec->inRenderContext.inPlaybackRatio),
									   fmt.inPixelFormat);
	AshenGetLog((*localRecH)->importerID, "sync", frameNum, AshenNowMs() - syncT0, 0,
				rec->inRenderContext.inIntent, w, h, (int)fmt.inPixelFormat);
	if (r == malNoError)
	{
		prSuiteError addRc = (*localRecH)->PPixCacheSuite->AddFrameToCache(
				(*localRecH)->importerID, 0, ppix, frameNum, NULL, NULL);
		AshenCacheAddLog((*localRecH)->importerID, frameNum, (int)addRc);
		*(rec->outFrame) = ppix;
		return aiNoError;
	}
	(*localRecH)->PPixSuite->Dispose(ppix);
	*(rec->outFrame) = NULL;
	return aiFrameNotFound;
}
