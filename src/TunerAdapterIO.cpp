#include "TunerAdapterIO.h"

namespace {
// How long to wait for the first bytes before declaring the channel dead.
// A demodulator needs to acquire lock before it emits anything, and on a
// weak signal that is genuinely slow - several seconds is normal, not a
// sign of failure.
const bigtime_t kTunerTimeout = 15000000;

const size_t kChunkSize = 32768;

// How much has to be buffered before BMediaFile is allowed to look at the
// stream. Identifying MPEG-2 TS means finding the 188-byte sync rhythm and
// then reading far enough in to see a PAT and a PMT, and the extractor will
// seek backwards over that window while it works. Handing it one chunk, the
// way an elementary-stream source can get away with, leaves it probing a
// stream that has barely started.
const size_t kProbeBytes = 256 * 1024;
}


TunerAdapterIO::TunerAdapterIO(Tuner* tuner, const BMessenger& nameTarget)
	:
	// Seekable, matching R World Radio's HlsAdapterIO. Declaring a live
	// source as streaming-only looks more honest, but BAdapterIO's backing
	// buffer is what makes backward seeks work at all, and the media
	// extractor seeks while sniffing - without it, PluginManager::CreateReader
	// segfaults before any plugin gets a look at the stream.
	BAdapterIO(B_MEDIA_STREAMING | B_MEDIA_SEEKABLE, kTunerTimeout),
	fTuner(tuner),
	fNameTarget(nameTarget),
	fInputAdapter(NULL),
	fWorkerThread(-1),
	fInitSem(create_sem(0, "tuner-adapter-init")),
	fInitReleased(false),
	fInitSucceeded(false),
	fStopRequested(0),
	fRunning(0)
{
}


TunerAdapterIO::~TunerAdapterIO()
{
	atomic_set(&fStopRequested, 1);
	if (fWorkerThread >= 0) {
		status_t exitValue;
		wait_for_thread(fWorkerThread, &exitValue);
	}
	delete_sem(fInitSem);
}


void
TunerAdapterIO::GetFlags(int32* flags) const
{
	// Live broadcast: no seeking backwards, unlike HlsAdapterIO which can
	// serve from its buffered segments. Telling the Media Kit the truth here
	// stops it trying to seek during format detection, which against a tuner
	// would just stall.
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

		fInputAdapter->Write(chunk, read);
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
}
