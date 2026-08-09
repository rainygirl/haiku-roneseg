#ifndef RONESEG_TUNER_H
#define RONESEG_TUNER_H

#include <SupportDefs.h>

#include <string>

// What a One-Seg front end has to be able to do, and nothing more.
//
// A One-Seg module is not a software radio: the OFDM demodulation happens in
// a fixed-function chip and what comes back over the bus is an already
// demodulated MPEG-2 transport stream. So the entire hardware surface is
// "point at a frequency" plus "hand me TS bytes", which is all this is.
//
// Two backends implement it:
//   FileTuner - replays a captured .ts at roughly broadcast rate. Everything
//               downstream of the tuner (demux, decode, sync, UI) is
//               exercised by this, with no hardware involved.
//   UsbTuner  - the real module, over the userland USB Kit.
class Tuner {
public:
	struct Status {
		bool	locked;
		int32	strength;	// 0-100, or -1 when the backend cannot tell
		int32	quality;	// 0-100, or -1 when the backend cannot tell

		Status() : locked(false), strength(-1), quality(-1) {}
	};

	virtual ~Tuner() {}

	// Acquires the device. On failure, LastError() explains why in a form
	// worth putting in front of a user.
	virtual status_t Open() = 0;
	virtual void Close() = 0;

	// Retunes and restarts the stream. frequencyHz is the channel centre -
	// One-Seg is the centre segment of the ISDB-T channel, so the centre
	// frequency is the same number for both.
	virtual status_t Tune(uint64 frequencyHz) = 0;

	virtual status_t GetStatus(Status* out) = 0;

	// Blocking. Returns bytes read, 0 on a timeout with no data (the caller
	// should keep going), or a negative status_t on a hard failure.
	virtual ssize_t Read(void* buffer, size_t size) = 0;

	// For the window title and the log: "captured.ts" or "Foo Bar (1234:5678)".
	virtual std::string Description() const = 0;

	const std::string& LastError() const { return fLastError; }

protected:
	void SetLastError(const std::string& error) { fLastError = error; }

private:
	std::string fLastError;
};

#endif
