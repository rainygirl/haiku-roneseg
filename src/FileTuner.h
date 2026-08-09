#ifndef RONESEG_FILE_TUNER_H
#define RONESEG_FILE_TUNER_H

#include <OS.h>

#include <stdio.h>

#include "Tuner.h"

// Replays a captured transport stream as if it were arriving off the air.
//
// This exists so the whole pipeline below the tuner - TS into BMediaFile,
// two decoded tracks, A/V sync, the video view, the channel UI - can be
// built and debugged before anyone knows what chip is inside the machine.
// It is also how you tell a decode problem apart from a reception problem
// later: if a capture plays here and the same channel does not play live,
// the fault is in front of the demodulator, not behind it.
//
// Reads are paced to a byte rate rather than free-running, because
// BMediaFile will happily consume a file as fast as the CPU allows and then
// starve, which looks nothing like a live tuner.
class FileTuner : public Tuner {
public:
	// Comfortably above what a One-Seg multiplex occupies (~416 kbit/s of
	// content, a little over 56 kB/s on the wire).
	//
	// The first version paced at exactly that nominal rate and it was wrong
	// in the one direction that matters: a capture whose real rate is a few
	// hundred bytes per second higher starves the decoder a little more with
	// every second that passes, which shows up as playback that runs, stalls,
	// runs again, with the audio callback filling each gap with silence at
	// callback rate - an audible chirp.
	//
	// Pacing here was only ever simulating live arrival, and it does not need
	// to be exact to do that: BAdapterIO is a buffer, not a pipe, so feeding
	// ahead of the decoder costs bounded memory and nothing else. Running at
	// several times the nominal rate keeps it fed no matter what the capture's
	// true rate is, while still bounding how hard this competes with the
	// decoder for a CPU that has one thread.
	static const size_t kDefaultByteRate = 4 * 56000;

	explicit FileTuner(const char* path, size_t byteRate = kDefaultByteRate);
	virtual ~FileTuner();

	virtual status_t Open();
	virtual void Close();
	virtual status_t Tune(uint64 frequencyHz);
	virtual status_t GetStatus(Status* out);
	virtual ssize_t Read(void* buffer, size_t size);
	virtual std::string Description() const;

private:
	std::string	fPath;
	size_t		fByteRate;
	FILE*		fFile;
	bigtime_t	fStartTime;
	off_t		fBytesRead;
};

#endif
