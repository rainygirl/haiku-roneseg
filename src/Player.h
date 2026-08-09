#ifndef RONESEG_PLAYER_H
#define RONESEG_PLAYER_H

#include <Locker.h>
#include <MediaDefs.h>
#include <Messenger.h>

#include <string>

class BMediaFile;
class BMediaTrack;
class BSoundPlayer;
class Tuner;
class TunerAdapterIO;
class VideoView;

// Decodes one tuned channel: transport stream in, picture on a VideoView and
// sound out of the mixer.
//
// The TS goes into BMediaFile unmodified and comes back out as two decoded
// tracks. Audio is pulled by BSoundPlayer; video is decoded on its own
// thread and presented against the audio clock.
//
// Why the audio clock rather than system_time(): a One-Seg multiplex is
// ~416 kbit/s of which video is maybe 128, and on this CPU the video decode
// is the part that can fall behind. Pacing video off the wall clock would
// let it drift away from sound that is playing correctly. Pacing it off how
// much audio the mixer has actually consumed keeps lip sync, and when the
// decoder cannot keep up it shows up as dropped frames - which is the right
// failure for a machine with no video acceleration.
class Player {
public:
	static const uint32 kStatusMessage = 'ROps';

	enum State {
		kTuning,
		kPlaying,
		kStopped,
		kError
	};

	// statusTarget receives kStatusMessage with:
	//   int32  "state"  - a State
	//   string "detail" - human-readable, on kError
	Player(const BMessenger& statusTarget, VideoView* videoView);
	~Player();

	// Does not take ownership of the tuner. Returns immediately; progress
	// arrives as messages.
	void Start(Tuner* tuner);
	void Stop();

	bool IsPlaying() const;

	// Frames the video decoder gave up on because the audio clock had
	// already passed them. Worth putting on screen: on this hardware it is
	// the number that tells you whether the machine is coping.
	uint32 DroppedFrames() const;

private:
	struct Session;

	static status_t SetupThreadEntry(void* cookie);
	static status_t VideoThreadEntry(void* cookie);
	static status_t AudioThreadEntry(void* cookie);
	static void PlayBufferProc(void* cookie, void* buffer, size_t size,
		const media_raw_audio_format& format);

	void RunSetup(Session* session);
	void RunVideo(Session* session);
	void RunAudio(Session* session);
	void EmitStatus(State state, const std::string& detail);
	void Teardown(Session* session);

	BMessenger		fStatusTarget;
	VideoView*		fVideoView;
	mutable BLocker	fMutex;
	Session*		fSession;
	uint64			fGeneration;
};

#endif
