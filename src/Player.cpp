#include "Player.h"

#include <Autolock.h>
#include <Bitmap.h>
#include <MediaFile.h>
#include <MediaTrack.h>
#include <SoundPlayer.h>

#include <new>
#include <stdio.h>
#include <string.h>

#include "Tuner.h"
#include "TunerAdapterIO.h"
#include "VideoView.h"

struct Player::Session {
	Player*				owner;
	Tuner*				tuner;
	TunerAdapterIO*		io;
	BMediaFile*			mediaFile;
	BMediaTrack*		videoTrack;
	BMediaTrack*		audioTrack;
	BSoundPlayer*		soundPlayer;
	thread_id			videoThread;
	// Stop() must not free the session out from under the setup thread,
	// which holds a raw pointer to it for as long as format detection takes
	// - and that is seconds, not microseconds, because it waits on the
	// tuner. Recorded before the thread is resumed so it is always readable.
	thread_id			setupThread;
	uint64				generation;

	media_format		videoFormat;
	media_raw_audio_format audioFormat;

	// Audio frames handed to the mixer. The video clock is derived from
	// this, so it is written only in the sound callback and read with
	// atomic_get64 from the video thread.
	int64				framesPlayed;
	int32				stopRequested;
	uint32				droppedFrames;

	// Decoded audio, produced by its own thread and consumed by the sound
	// callback.
	//
	// The obvious implementation calls ReadFrames() straight from the
	// BSoundPlayer callback, and it does not work here for two compounding
	// reasons. The codec's natural unit (1024 frames for AAC) is not the size
	// the callback asks for, so a single read leaves a gap in every buffer -
	// audible as a steady chirp. And both tracks come from one MediaExtractor,
	// which serialises reads, so decoding audio in the callback puts it behind
	// a lock the video thread holds constantly: the callback misses its
	// deadline and the mixer plays silence.
	//
	// A callback that only copies out of a ring buffer has neither problem.
	uint8*				audioRing;
	size_t				audioRingSize;
	int64				audioWritten;	// monotonic byte counters, not indices
	int64				audioRead;
	thread_id			audioThread;
	size_t				audioFrameSize;
	int32				audioUnderruns;

	Session()
		:
		owner(NULL), tuner(NULL), io(NULL), mediaFile(NULL), videoTrack(NULL),
		audioTrack(NULL), soundPlayer(NULL), videoThread(-1), setupThread(-1),
		generation(0), framesPlayed(0), stopRequested(0), droppedFrames(0),
		audioRing(NULL), audioRingSize(0), audioWritten(0), audioRead(0),
		audioThread(-1), audioFrameSize(0), audioUnderruns(0)
	{
	}
};


Player::Player(const BMessenger& statusTarget, VideoView* videoView)
	:
	fStatusTarget(statusTarget),
	fVideoView(videoView),
	fMutex("roneseg-player"),
	fSession(NULL),
	fGeneration(0)
{
}


Player::~Player()
{
	Stop();
}


bool
Player::IsPlaying() const
{
	BAutolock lock(fMutex);
	return fSession != NULL && fSession->soundPlayer != NULL;
}


uint32
Player::DroppedFrames() const
{
	BAutolock lock(fMutex);
	return fSession != NULL ? fSession->droppedFrames : 0;
}


void
Player::EmitStatus(State state, const std::string& detail)
{
	BMessage message(kStatusMessage);
	message.AddInt32("state", state);
	message.AddString("detail", detail.c_str());
	fStatusTarget.SendMessage(&message);
}


void
Player::Start(Tuner* tuner)
{
	Stop();

	Session* session = new(std::nothrow) Session();
	if (session == NULL) {
		EmitStatus(kError, "out of memory");
		return;
	}

	fMutex.Lock();
	session->owner = this;
	session->tuner = tuner;
	session->generation = ++fGeneration;
	fSession = session;
	fMutex.Unlock();

	EmitStatus(kTuning, "");

	// Format detection reads from the stream and can block for as long as
	// the demodulator takes to lock, so it must not run on the caller's
	// thread - which is the window's.
	thread_id setup = spawn_thread(&Player::SetupThreadEntry,
		"roneseg-setup", B_NORMAL_PRIORITY, session);
	if (setup < 0) {
		EmitStatus(kError, "could not start the decoder thread");
		return;
	}
	// Recorded before resuming, so Stop() can never see -1 for a thread that
	// is already running.
	session->setupThread = setup;
	resume_thread(setup);
}


status_t
Player::SetupThreadEntry(void* cookie)
{
	Session* session = static_cast<Session*>(cookie);
	session->owner->RunSetup(session);
	return B_OK;
}


void
Player::RunSetup(Session* session)
{
	session->io = new(std::nothrow) TunerAdapterIO(session->tuner,
		fStatusTarget);
	if (session->io == NULL) {
		EmitStatus(kError, "out of memory");
		return;
	}

	status_t status = session->io->Open();
	if (status != B_OK) {
		std::string detail = session->io->InitError();
		if (detail.empty())
			detail = "no transport stream from the tuner";
		EmitStatus(kError, detail);
		Teardown(session);
		return;
	}

	session->mediaFile = new(std::nothrow) BMediaFile(session->io);
	if (session->mediaFile == NULL
		|| session->mediaFile->InitCheck() != B_OK) {
		// Almost always means the ffmpeg media add-on did not recognise the
		// stream. Worth naming, because the usual cause is that add-on being
		// absent rather than the stream being bad.
		EmitStatus(kError, "the Media Kit could not read this stream - is the "
			"ffmpeg media add-on installed?");
		Teardown(session);
		return;
	}

	for (int32 i = 0; i < session->mediaFile->CountTracks(); i++) {
		BMediaTrack* track = session->mediaFile->TrackAt(i);
		if (track == NULL)
			continue;

		media_format format;
		// Cast through void*: media_format has non-trivial copy assignment,
		// which -Wclass-memaccess warns about, but zeroing it is exactly
		// what the Media Kit expects before a DecodedFormat() negotiation.
		memset((void*)&format, 0, sizeof(format));
		if (track->EncodedFormat(&format) != B_OK) {
			session->mediaFile->ReleaseTrack(track);
			continue;
		}

		if (format.IsVideo() && session->videoTrack == NULL) {
			// Cast through void*: media_format has non-trivial copy assignment,
		// which -Wclass-memaccess warns about, but zeroing it is exactly
		// what the Media Kit expects before a DecodedFormat() negotiation.
		memset((void*)&format, 0, sizeof(format));
			format.type = B_MEDIA_RAW_VIDEO;
			// B_RGB32 because that is what BBitmap draws without a further
			// conversion pass. Asking for the decoder's native YCbCr and
			// converting here would mean writing a colourspace converter and
			// running it on a CPU that has nothing to spare.
			format.u.raw_video.display.format = B_RGB32;
			if (track->DecodedFormat(&format) == B_OK) {
				session->videoTrack = track;
				session->videoFormat = format;
				continue;
			}
		} else if (format.IsAudio() && session->audioTrack == NULL) {
			// Cast through void*: media_format has non-trivial copy assignment,
		// which -Wclass-memaccess warns about, but zeroing it is exactly
		// what the Media Kit expects before a DecodedFormat() negotiation.
		memset((void*)&format, 0, sizeof(format));
			format.type = B_MEDIA_RAW_AUDIO;
			format.u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
			format.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
			if (track->DecodedFormat(&format) == B_OK) {
				session->audioTrack = track;
				session->audioFormat = format.u.raw_audio;
				continue;
			}
		}

		session->mediaFile->ReleaseTrack(track);
	}

	if (session->videoTrack == NULL && session->audioTrack == NULL) {
		EmitStatus(kError, "the stream carries no track this machine can "
			"decode");
		Teardown(session);
		return;
	}

	if (session->audioTrack != NULL) {
		session->audioFrameSize = (session->audioFormat.format
				& media_raw_audio_format::B_AUDIO_SIZE_MASK)
			* session->audioFormat.channel_count;
		if (session->audioFrameSize == 0)
			session->audioFrameSize = 4;

		// About a second of decoded audio. Enough that the decoder can fall
		// behind for a moment - which on this CPU it will, whenever the video
		// thread has a busy frame - without the mixer noticing.
		session->audioRingSize = (size_t)(session->audioFormat.frame_rate
			* session->audioFrameSize);
		if (session->audioRingSize < 262144)
			session->audioRingSize = 262144;
		session->audioRing = new(std::nothrow) uint8[session->audioRingSize];
		if (session->audioRing == NULL) {
			EmitStatus(kError, "out of memory");
			Teardown(session);
			return;
		}

		// Above the video thread, which runs below normal. There is one CPU
		// here - the second logical processor never comes up on this unit -
		// so these two threads are in direct competition for it, and the
		// contest has a right answer: a late video frame is a lower frame
		// rate, a late audio buffer is an audible gap. At equal priority the
		// video thread, which never blocks, starves the audio decoder and
		// the mixer plays the gaps.
		session->audioThread = spawn_thread(&Player::AudioThreadEntry,
			"roneseg-audio", B_URGENT_DISPLAY_PRIORITY, session);
		if (session->audioThread >= 0)
			resume_thread(session->audioThread);

		session->soundPlayer = new(std::nothrow) BSoundPlayer(
			&session->audioFormat, "R One-Seg", &Player::PlayBufferProc,
			NULL, session);
		if (session->soundPlayer != NULL
			&& session->soundPlayer->InitCheck() == B_OK) {
			// Explicitly, before Start(). A BSoundPlayer does not reliably
			// come up at full volume on its own, and the failure mode is a
			// player that runs perfectly and produces nothing audible -
			// indistinguishable from a decode problem from the outside.
			// R World Radio's RadioPlayer sets this for the same reason.
			session->soundPlayer->SetVolume(1.0);
			session->soundPlayer->Start();
			session->soundPlayer->SetHasData(true);
		} else {
			// Worth saying out loud rather than silently continuing with
			// picture and no sound.
			fprintf(stderr, "roneseg: could not open audio output (%s)\n",
				session->soundPlayer != NULL
					? strerror(session->soundPlayer->InitCheck())
					: "allocation failed");
			delete session->soundPlayer;
			session->soundPlayer = NULL;
		}
	}

	if (session->videoTrack != NULL) {
		session->videoThread = spawn_thread(&Player::VideoThreadEntry,
			"roneseg-video", B_LOW_PRIORITY, session);
		if (session->videoThread >= 0)
			resume_thread(session->videoThread);
	}

	EmitStatus(kPlaying, session->tuner->Description());
}


void
Player::PlayBufferProc(void* cookie, void* buffer, size_t size,
	const media_raw_audio_format& format)
{
	Session* session = static_cast<Session*>(cookie);
	if (session->audioTrack == NULL || atomic_get(&session->stopRequested)) {
		memset(buffer, 0, size);
		return;
	}

	size_t frameSize = (format.format & media_raw_audio_format::B_AUDIO_SIZE_MASK)
		* format.channel_count;
	if (frameSize == 0) {
		memset(buffer, 0, size);
		return;
	}

	// The mixer plays this whole buffer whatever we manage to fill it with,
	// so the clock advances by its full length - otherwise a short decode
	// would stall the video thread's notion of time instead of just being
	// quiet.
	atomic_add64(&session->framesPlayed, size / frameSize);

	if (session->audioRing == NULL) {
		memset(buffer, 0, size);
		return;
	}

	// Nothing here blocks, allocates, or decodes: this runs on the mixer's
	// thread and every microsecond spent in it is a microsecond closer to an
	// underrun.
	int64 available = atomic_get64(&session->audioWritten)
		- atomic_get64(&session->audioRead);
	if (available < 0)
		available = 0;
	size_t take = available > (int64)size ? size : (size_t)available;

	// Counted, not just papered over: an underrun here is the chirp, and
	// knowing whether there are none, a few, or thousands is the difference
	// between "fixed" and "less bad".
	if (take < size)
		atomic_add(&session->audioUnderruns, 1);

	uint8* out = static_cast<uint8*>(buffer);
	size_t offset = (size_t)(atomic_get64(&session->audioRead)
		% (int64)session->audioRingSize);

	size_t first = take;
	if (offset + first > session->audioRingSize)
		first = session->audioRingSize - offset;
	if (first > 0)
		memcpy(out, session->audioRing + offset, first);
	if (take > first)
		memcpy(out + first, session->audioRing, take - first);

	atomic_add64(&session->audioRead, (int64)take);

	if (take < size)
		memset(out + take, 0, size - take);
}


status_t
Player::AudioThreadEntry(void* cookie)
{
	Session* session = static_cast<Session*>(cookie);
	session->owner->RunAudio(session);
	return B_OK;
}


void
Player::RunAudio(Session* session)
{
	// ReadFrames() takes no size argument and writes as much as the decoder's
	// own output buffer holds, so the scratch has to be at least that big.
	size_t scratchSize = session->audioFormat.buffer_size;
	if (scratchSize < 65536)
		scratchSize = 65536;

	uint8* scratch = new(std::nothrow) uint8[scratchSize];
	if (scratch == NULL)
		return;

	fprintf(stderr, "roneseg: audio %g Hz, %" B_PRIu32 " ch, frame %zu bytes, "
		"ring %zu bytes, scratch %zu bytes\n",
		session->audioFormat.frame_rate, session->audioFormat.channel_count,
		session->audioFrameSize, session->audioRingSize, scratchSize);

	int32 failures = 0;
	int32 reports = 0;

	while (atomic_get(&session->stopRequested) == 0) {
		int64 used = atomic_get64(&session->audioWritten)
			- atomic_get64(&session->audioRead);
		if ((size_t)used + scratchSize > session->audioRingSize) {
			// Ring is full enough. This is the normal steady state: the
			// decoder is faster than realtime and spends most of its life
			// here, which is exactly what leaves the CPU to the video thread.
			snooze(10000);
			continue;
		}

		int64 got = 0;
		media_header header;
		status_t status = session->audioTrack->ReadFrames(scratch, &got,
			&header);
		if (status != B_OK || got <= 0) {
			// Not the end of anything. A capture that loops, a broadcast
			// with a discontinuity, or a decoder that wants more input all
			// surface here, and the first version treated any of them as
			// fatal: the thread exited, the ring stayed empty, and audio was
			// silent for the rest of the session with no way back. Retry.
			if (++failures % 200 == 1) {
				fprintf(stderr, "roneseg: audio decode returned %s "
					"(%" B_PRId64 " frames), retrying\n",
					strerror(status), got);
			}
			snooze(20000);
			continue;
		}
		failures = 0;

		if (++reports % 200 == 0) {
			fprintf(stderr, "roneseg: ring %d%% full, %" B_PRId32
				" underruns, %" B_PRIu32 " video frames dropped\n",
				(int)(((atomic_get64(&session->audioWritten)
					- atomic_get64(&session->audioRead)) * 100)
					/ (int64)session->audioRingSize),
				atomic_get(&session->audioUnderruns),
				session->droppedFrames);
		}

		size_t bytes = (size_t)got * session->audioFrameSize;
		if (bytes > session->audioRingSize)
			bytes = session->audioRingSize;

		size_t offset = (size_t)(atomic_get64(&session->audioWritten)
			% (int64)session->audioRingSize);
		size_t first = bytes;
		if (offset + first > session->audioRingSize)
			first = session->audioRingSize - offset;
		memcpy(session->audioRing + offset, scratch, first);
		if (bytes > first)
			memcpy(session->audioRing, scratch + first, bytes - first);

		atomic_add64(&session->audioWritten, (int64)bytes);
	}

	delete[] scratch;
}


status_t
Player::VideoThreadEntry(void* cookie)
{
	Session* session = static_cast<Session*>(cookie);
	session->owner->RunVideo(session);
	return B_OK;
}


void
Player::RunVideo(Session* session)
{
	uint32 width = session->videoFormat.u.raw_video.display.line_width;
	uint32 height = session->videoFormat.u.raw_video.display.line_count;
	if (width == 0 || height == 0) {
		width = 320;
		height = 240;
	}

	BRect bounds(0, 0, width - 1, height - 1);
	// One scratch bitmap that the decoder writes into, copied per presented
	// frame. Decoding straight into the bitmap handed to the view would mean
	// the decoder overwriting a bitmap app_server is still reading - the same
	// race the VAIO P patch set fixed in CodyCam's VideoConsumer.
	BBitmap* scratch = new(std::nothrow) BBitmap(bounds, B_RGB32);
	if (scratch == NULL || scratch->InitCheck() != B_OK) {
		delete scratch;
		return;
	}

	const bigtime_t wallStart = system_time();

	// Stream time and playback time do not share an origin. A transport
	// stream's PTS starts at whatever the muxer felt like - ffmpeg uses 1.4s,
	// a broadcaster uses wall-clock-derived values in the tens of hours -
	// while the audio clock below starts at zero when BSoundPlayer does.
	// Comparing the two directly makes every frame look hopelessly late, so
	// every frame gets dropped and the picture freezes while sound carries on
	// perfectly. The first frame decoded defines the origin instead.
	bigtime_t origin = -1;

	while (atomic_get(&session->stopRequested) == 0) {
		int64 frameCount = 0;
		media_header header;
		status_t status = session->videoTrack->ReadFrames(scratch->Bits(),
			&frameCount, &header);
		if (status != B_OK)
			break;

		// Where playback actually is, in stream time.
		bigtime_t now;
		if (session->soundPlayer != NULL
			&& session->audioFormat.frame_rate > 0) {
			int64 played = atomic_get64(&session->framesPlayed);
			now = static_cast<bigtime_t>(
				(played * 1000000.0) / session->audioFormat.frame_rate);
		} else {
			now = system_time() - wallStart;
		}

		if (origin < 0)
			origin = header.start_time;

		bigtime_t due = header.start_time - origin;

		if (due > now) {
			bigtime_t wait = due - now;
			// Cap the wait. A bad PTS, or the discontinuity right after a
			// retune, would otherwise park this thread on a timestamp that
			// is never coming.
			if (wait > 500000)
				wait = 500000;
			snooze(wait);
		} else if (now - due > 1000000) {
			// A whole second behind is not a slow decoder, it is the two
			// clocks having genuinely parted company - a PTS discontinuity,
			// or a retune. Rebase onto the current position.
			origin = header.start_time - now;
			session->droppedFrames++;
		}

		// Everything else is presented even when it is late.
		//
		// The first version dropped anything more than 200ms behind, which on
		// this machine meant dropping almost everything: decode and
		// compositing are both entirely in software here, so 15fps at 320x240
		// is close to what the CPU can manage and frames are routinely a
		// little late. Dropping them turned "slightly too slow" into one
		// visible frame every couple of seconds. Showing a late frame just
		// looks like a lower frame rate, which is the honest result.
		fVideoView->SetFrame(scratch);
	}

	delete scratch;
}


void
Player::Teardown(Session* session)
{
	atomic_set(&session->stopRequested, 1);

	if (session->soundPlayer != NULL) {
		session->soundPlayer->SetHasData(false);
		session->soundPlayer->Stop();
		delete session->soundPlayer;
		session->soundPlayer = NULL;
	}

	if (session->videoThread >= 0) {
		status_t exitValue;
		wait_for_thread(session->videoThread, &exitValue);
		session->videoThread = -1;
	}

	// Both decode threads have to be gone before the tracks they read from
	// are released, and before the ring they write into is freed.
	if (session->audioThread >= 0) {
		status_t exitValue;
		wait_for_thread(session->audioThread, &exitValue);
		session->audioThread = -1;
	}

	// Tracks belong to the BMediaFile and are released through it; the
	// BMediaFile must go before the BDataIO it reads from.
	if (session->mediaFile != NULL) {
		if (session->videoTrack != NULL)
			session->mediaFile->ReleaseTrack(session->videoTrack);
		if (session->audioTrack != NULL)
			session->mediaFile->ReleaseTrack(session->audioTrack);
		delete session->mediaFile;
		session->mediaFile = NULL;
	}
	session->videoTrack = NULL;
	session->audioTrack = NULL;

	delete session->io;
	session->io = NULL;

	// After the sound player is stopped, so the callback cannot be running.
	delete[] session->audioRing;
	session->audioRing = NULL;
	session->audioWritten = 0;
	session->audioRead = 0;
}


void
Player::Stop()
{
	fMutex.Lock();
	Session* session = fSession;
	fSession = NULL;
	fMutex.Unlock();

	if (session == NULL)
		return;

	// Tell everything to wind down, then wait for the setup thread before
	// touching the session: it is still reading these fields while
	// BMediaFile probes the stream, and freeing underneath it is what turns
	// a stop into a crash.
	atomic_set(&session->stopRequested, 1);
	if (session->setupThread >= 0) {
		status_t exitValue;
		wait_for_thread(session->setupThread, &exitValue);
		session->setupThread = -1;
	}

	Teardown(session);
	delete session;

	if (fVideoView != NULL) {
		fVideoView->Clear();
		fVideoView->SetPlaceholder("stopped");
	}
	EmitStatus(kStopped, "");
}
