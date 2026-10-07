#ifndef SYMPATHY_H
#include "sympathy.h"
#endif

#include <list>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>

PREMPLUGENTRY xAsyncImportEntry(int inSelector, void *inParam);

enum AsyncJobState { kAsyncPending = 0, kAsyncInProgress = 1, kAsyncDone = 2, kAsyncError = 3 };

struct AsyncJob
{
	csSDK_int32		frameNum;
	PPixHand		ppix;
	char*			buffer;
	csSDK_int32		rowBytes;
	int				width, height;
	PrPixelFormat	pixelFormat;
	int				state;
	int				lowresHint;
	int				waiters;
	bool			abandoned;
};

class AshenAsyncImporter
{
public:
	AshenAsyncImporter(imStdParms *stdParms, AshenLocalH inRecH);
	~AshenAsyncImporter();
	int OnInitiateAsyncRead(imSourceVideoRec& inSourceRec);
	int OnCancelAsyncRead(imSourceVideoRec& inSourceRec);
	int OnFlush();
	int OnGetFrame(imSourceVideoRec* inFrameRec);

private:
	void		WorkerLoop();
	csSDK_int32	FrameOf(const imSourceVideoRec& rec) const;

	AshenLocalH			localRecH;
	std::list<AsyncJob*>		jobs;
	std::mutex					mtx;
	std::condition_variable		cvWork;
	std::condition_variable		cvDone;
	std::vector<std::thread>	workers;
	bool						stop;
};
