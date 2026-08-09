#ifndef RONESEG_TUNER_ADAPTER_IO_H
#define RONESEG_TUNER_ADAPTER_IO_H

#include <AdapterIO.h>
#include <Messenger.h>
#include <OS.h>

#include "SiParser.h"
#include "Tuner.h"

// Feeds a tuner's transport stream into BMediaFile.
//
// Same shape as R World Radio's HlsAdapterIO, and for the same reason: it is
// how Haiku's own http_streamer add-on bridges a live byte source into the
// Media Kit. A worker thread pulls from the Tuner and writes through
// BInputAdapter; BMediaFile sniffs the result and picks decoders.
//
// One deliberate difference from HlsAdapterIO: nothing is demuxed here. The
// raw TS goes in as it comes off the tuner. R World Radio stripped its
// streams down to a bare elementary stream because it only ever had one
// audio track to deal with, but One-Seg is video *and* audio, and the only
// thing holding those two in sync is the PTS/PCR timing in the TS itself.
// Demuxing here would throw that away and make A/V sync this app's problem
// instead of the decoder's.
//
// Ownership: BMediaFile(BDataIO*) does not delete its source. Open() this
// before handing it over, and delete it after the BMediaFile.
class TunerAdapterIO : public BAdapterIO {
public:
	// Sent to nameTarget as new service names arrive out of the SDT:
	//   string "name" - the first service's name, UTF-8
	static const uint32 kServiceNameMessage = 'ROsn';

	// Does not take ownership of the tuner; Player outlives this.
	TunerAdapterIO(Tuner* tuner, const BMessenger& nameTarget);
	~TunerAdapterIO();

	void GetFlags(int32* flags) const;
	status_t Open();

	bool IsRunning() const;
	const std::string& InitError() const { return fInitError; }

private:
	static status_t WorkerThreadEntry(void* cookie);
	void RunWorker();
	void ReleaseInitOnce(bool success, const std::string& error = std::string());

	Tuner*			fTuner;
	BMessenger		fNameTarget;
	SiParser		fSiParser;
	std::string		fInitError;
	BInputAdapter*	fInputAdapter;
	thread_id		fWorkerThread;
	sem_id			fInitSem;
	bool			fInitReleased;
	bool			fInitSucceeded;

	int32			fStopRequested;
	mutable int32	fRunning;
};

#endif
