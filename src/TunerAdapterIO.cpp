#include "TunerAdapterIO.h"
#include <Autolock.h>
#include <Locker.h>
#include <algorithm>
#include <string.h>
#include <vector>

namespace {
// How long to wait for the first bytes before declaring the channel dead.
// A demodulator needs to acquire lock before it emits anything, and on a
// weak signal that is genuinely slow - several seconds is normal, not a
// sign of failure.
const bigtime_t kTunerTimeout = 45000000;

const size_t kChunkSize = 32768;

// How much has to be buffered before BMediaFile is allowed to look at the
// stream. Identifying MPEG-2 TS means finding the 188-byte sync rhythm and
// then reading far enough in to see a PAT and a PMT, and the extractor will
// seek backwards over that window while it works. Handing it one chunk, the
// way an elementary-stream source can get away with, leaves it probing a
// stream that has barely started.
const size_t kProbeBytes = 128 * 1024;

// BAdapterIO's default BMallocIO grows for the lifetime of a broadcast.
// Retain a bounded backward-seek window for format probing instead.
class StreamBuffer : public BPositionIO {
public:
	StreamBuffer() : fData(4 * 1024 * 1024), fEnd(0), fPosition(0),
		fLock("oneseg stream buffer") {}
	ssize_t ReadAt(off_t position, void* data, size_t size)
	{
		BAutolock lock(fLock);
		if (position < First() || position > fEnd) return B_BAD_VALUE;
		size = std::min(size, (size_t)(fEnd - position));
		size_t offset = (size_t)(position % fData.size());
		size_t first = std::min(size, fData.size() - offset);
		if (first) memcpy(data, &fData[offset], first);
		if (size > first) memcpy((uint8*)data + first, &fData[0], size - first);
		return size;
	}
	ssize_t WriteAt(off_t position, const void* data, size_t size)
	{
		BAutolock lock(fLock);
		if (position != fEnd || size > fData.size()) return B_BAD_VALUE;
		size_t offset = (size_t)(position % fData.size());
		size_t first = std::min(size, fData.size() - offset);
		if (first) memcpy(&fData[offset], data, first);
		if (size > first) memcpy(&fData[0], (const uint8*)data + first, size - first);
		fEnd += size;
		return size;
	}
	off_t Seek(off_t offset, uint32 mode)
	{
		BAutolock lock(fLock);
		off_t position = mode == SEEK_SET ? offset
			: mode == SEEK_CUR ? fPosition + offset : fEnd + offset;
		if (position < First() || position > fEnd) return B_BAD_VALUE;
		return fPosition = position;
	}
	off_t Position() const { BAutolock lock(fLock); return fPosition; }
	status_t GetSize(off_t* size) const
		{ BAutolock lock(fLock); *size = fEnd; return B_OK; }
	status_t SetSize(off_t size)
	{
		BAutolock lock(fLock);
		if (size != 0) return B_NOT_SUPPORTED;
		fEnd = fPosition = 0;
		return B_OK;
	}
private:
	off_t First() const { return std::max((off_t)0, fEnd - (off_t)fData.size()); }
	std::vector<uint8> fData;
	off_t fEnd, fPosition;
	mutable BLocker fLock;
};
}


TunerAdapterIO::TunerAdapterIO(Tuner* tuner, const BMessenger& nameTarget, uint64 generation)
	:
	// Seekable, matching R World Radio's HlsAdapterIO. Declaring a live
	// source as streaming-only looks more honest, but BAdapterIO's backing
	// buffer is what makes backward seeks work at all, and the media
	// extractor seeks while sniffing - without it, PluginManager::CreateReader
	// segfaults before any plugin gets a look at the stream.
	BAdapterIO(B_MEDIA_STREAMING | B_MEDIA_SEEKABLE, kTunerTimeout),
	fTuner(tuner),
	fNameTarget(nameTarget),
	fGeneration(generation),
	fInputAdapter(NULL),
	fWorkerThread(-1),
	fInitSem(create_sem(0, "tuner-adapter-init")),
	fInitReleased(false),
	fInitSucceeded(false),
	fStopRequested(0),
	fRunning(0)
{
	SetBuffer(new StreamBuffer());
}


TunerAdapterIO::~TunerAdapterIO()
{
	Stop();
	delete_sem(fInitSem);
}


void
TunerAdapterIO::Stop()
{
	atomic_set(&fStopRequested, 1);
	atomic_set(&fRunning, 0);
	if (fWorkerThread >= 0) {
		status_t exitValue;
		wait_for_thread(fWorkerThread, &exitValue);
		fWorkerThread = -1;
	}
}


void
TunerAdapterIO::GetFlags(int32* flags) const
{
	// Format detection revisits bytes already read. The bounded ring keeps
	// that recent history, without advertising arbitrary forward seeking.
	*flags = B_MEDIA_STREAMING | B_MEDIA_SEEK_BACKWARD;
}


status_t
TunerAdapterIO::Open()
{
	fInputAdapter = BuildInputAdapter();

	fWorkerThread = spawn_thread(&TunerAdapterIO::WorkerThreadEntry,
		"tuner-worker", B_NORMAL_PRIORITY, this);
	if (fWorkerThread < 0)
		return fWorkerThread;
	resume_thread(fWorkerThread);

	status_t err = acquire_sem_etc(fInitSem, 1, B_RELATIVE_TIMEOUT,
		kTunerTimeout);
	if (err != B_OK)
		return err;
	if (!fInitSucceeded)
		return B_ERROR;

	return BAdapterIO::Open();
}


bool
TunerAdapterIO::IsRunning() const
{
	return atomic_get(&fRunning) != 0;
}


status_t
TunerAdapterIO::WorkerThreadEntry(void* cookie)
{
	static_cast<TunerAdapterIO*>(cookie)->RunWorker();
	return B_OK;
}


void
TunerAdapterIO::ReleaseInitOnce(bool success, const std::string& error)
{
	if (fInitReleased)
		return;
	fInitReleased = true;
	fInitSucceeded = success;
	if (!success)
		fInitError = error;
	release_sem(fInitSem);
}


void
TunerAdapterIO::RunWorker()
{
	atomic_set(&fRunning, 1);

	uint8* chunk = new(std::nothrow) uint8[kChunkSize];
	if (chunk == NULL) {
		ReleaseInitOnce(false, "out of memory");
		atomic_set(&fRunning, 0);
		return;
	}

	std::string lastIssue = "the tuner produced no data";
	size_t written = 0;

	while (atomic_get(&fStopRequested) == 0) {
		// File replay can arrive faster than playback. Keep it within the
		// retained seek window; live reception has its own bounded USB queue.
		if ((off_t)written > Position() + 2 * 1024 * 1024) {
			snooze(10000);
			continue;
		}
		ssize_t read = fTuner->Read(chunk, kChunkSize);
		if (read < 0) {
			lastIssue = fTuner->LastError().empty()
				? std::string("read failed") : fTuner->LastError();
			break;
		}
		if (read == 0) {
			// Backend has nothing yet - a file backend pacing itself, or a
			// demodulator that has not locked. Neither is an error.
			continue;
		}

		if (fInputAdapter->Write(chunk, read) != read) {
			lastIssue = "could not buffer the transport stream";
			break;
		}
		written += read;
		if (written >= kProbeBytes)
			ReleaseInitOnce(true);

		// Tap the same bytes on the way past for the service name. The SDT
		// repeats every couple of seconds, so this arrives a little after
		// the picture does rather than with it.
		if (fSiParser.Feed(chunk, read)) {
			BMessage message(kServiceNameMessage);
			message.AddString("name", fSiParser.PrimaryName().c_str());
			fNameTarget.SendMessage(&message);
		}
	}

	delete[] chunk;
	ReleaseInitOnce(false, lastIssue); // no-op once data has flowed
	atomic_set(&fRunning, 0);
	if (!atomic_get(&fStopRequested) && fInitSucceeded) {
		BMessage error(kStreamErrorMessage);
		error.AddInt64("generation", fGeneration);
		error.AddString("detail", lastIssue.c_str());
		fNameTarget.SendMessage(&error);
	}
}
