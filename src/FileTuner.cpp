#include "FileTuner.h"

#include <errno.h>
#include <string.h>

FileTuner::FileTuner(const char* path, size_t byteRate)
	:
	fPath(path),
	fByteRate(byteRate > 0 ? byteRate : kDefaultByteRate),
	fFile(NULL),
	fStartTime(0),
	fBytesRead(0)
{
}


FileTuner::~FileTuner()
{
	Close();
}


status_t
FileTuner::Open()
{
	Close();

	fFile = fopen(fPath.c_str(), "rb");
	if (fFile == NULL) {
		SetLastError("could not open " + fPath + ": " + strerror(errno));
		return B_ENTRY_NOT_FOUND;
	}

	fStartTime = system_time();
	fBytesRead = 0;
	return B_OK;
}


void
FileTuner::Close()
{
	if (fFile != NULL) {
		fclose(fFile);
		fFile = NULL;
	}
}


status_t
FileTuner::Tune(uint64 frequencyHz)
{
	// A capture is one channel by definition. Rewinding on every "tune" at
	// least makes channel-up/down do something visible while testing the UI.
	(void)frequencyHz;
	if (fFile == NULL)
		return B_NO_INIT;

	rewind(fFile);
	fStartTime = system_time();
	fBytesRead = 0;
	return B_OK;
}


status_t
FileTuner::GetStatus(Status* out)
{
	if (out == NULL)
		return B_BAD_VALUE;

	out->locked = fFile != NULL && feof(fFile) == 0;
	// Deliberately not faking a signal reading. -1 means "the backend
	// cannot tell", and the UI shows a dash rather than a bar - a capture
	// has no signal to report and inventing 100% would make the meter lie
	// in exactly the situation where you are trying to trust it.
	out->strength = -1;
	out->quality = -1;
	return B_OK;
}


ssize_t
FileTuner::Read(void* buffer, size_t size)
{
	if (fFile == NULL)
		return B_NO_INIT;

	// Hand back only as much as the elapsed time says has "arrived".
	bigtime_t elapsed = system_time() - fStartTime;
	off_t due = static_cast<off_t>(
		(static_cast<int64>(fByteRate) * elapsed) / 1000000LL);
	if (due <= fBytesRead) {
		snooze(20000);
		return 0;
	}

	size_t want = size;
	if (static_cast<off_t>(want) > due - fBytesRead)
		want = static_cast<size_t>(due - fBytesRead);

	size_t got = fread(buffer, 1, want, fFile);
	if (got == 0) {
		if (feof(fFile) != 0) {
			// Loop, so leaving it running overnight while working on the
			// video path does not silently stop after a few minutes.
			rewind(fFile);
			fStartTime = system_time();
			fBytesRead = 0;
			return 0;
		}
		SetLastError("read error on " + fPath);
		return B_IO_ERROR;
	}

	fBytesRead += got;
	return static_cast<ssize_t>(got);
}


std::string
FileTuner::Description() const
{
	return fPath;
}
