#include "LeiraDecoder.h"
#include "UsbTuner.h"
#include "DataFiles.h"

#include <FindDirectory.h>
#include <Path.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static uint32 ReadLE(const uint8* p, int count)
{
	uint32 value = 0;
	for (int i = 0; i < count; i++) value |= (uint32)p[i] << (8 * i);
	return value;
}

LeiraDecoder::LeiraDecoder(UsbTuner* tuner, const std::string& replay)
	: fTuner(tuner), fSocket(-1), fPid(-1), fCancelled(0), fReplay(replay),
	  fInitialized(false), fClean(true)
{
}

LeiraDecoder::~LeiraDecoder()
{
	Close();
}

void LeiraDecoder::Cancel()
{
	atomic_set(&fCancelled, 1);
}

bool LeiraDecoder::Resume()
{
	// Only after the previous reader has joined. An interrupted exchange
	// cannot be resumed: a partial response may still be in the pipe.
	if (!fInitialized || !fClean) return false;
	atomic_set(&fCancelled, 0);
	return true;
}

void LeiraDecoder::Close()
{
	Cancel();
	if (fSocket >= 0) close(fSocket);
	fSocket = -1;
	if (fPid > 0) {
		int status;
		if (waitpid(fPid, &status, WNOHANG) == 0) {
			kill(fPid, SIGTERM);
			for (int i = 0; i < 20; i++) {
				if (waitpid(fPid, &status, WNOHANG) != 0) {
					fPid = -1;
					return;
				}
				snooze(10000);
			}
			kill(fPid, SIGKILL);
			waitpid(fPid, &status, 0);
		}
	}
	fPid = -1;
}

bool LeiraDecoder::Transfer(void* data, size_t size, bool writing, bigtime_t deadline)
{
	uint8* p = (uint8*)data;
	while (size > 0 && !atomic_get(&fCancelled)) {
		if (system_time() >= deadline) {
			fError = "Local decoder timed out";
			return false;
		}
		pollfd fd = {fSocket, (short)(writing ? POLLOUT : POLLIN), 0};
		int ready = poll(&fd, 1, 100);
		if (ready < 0 && errno == EINTR) continue;
		if (ready == 0) continue;
		if (ready < 0 || (fd.revents & (POLLERR | POLLNVAL))) break;
		ssize_t count = writing ? send(fSocket, p, size, MSG_NOSIGNAL)
			: recv(fSocket, p, size, 0);
		if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
		if (count <= 0) break;
		p += count;
		size -= count;
	}
	if (size == 0) return true;
	fError = atomic_get(&fCancelled) ? "Playback cancelled" : "Local decoder closed unexpectedly";
	return false;
}

status_t LeiraDecoder::Send(uint8 kind, const void* data, size_t size)
{
	uint8 header[5] = {kind, (uint8)size, (uint8)(size >> 8),
		(uint8)(size >> 16), (uint8)(size >> 24)};
	bigtime_t deadline = system_time() + 3000000;
	return Transfer(header, 5, true, deadline)
		&& Transfer((void*)data, size, true, deadline) ? B_OK : B_IO_ERROR;
}

status_t LeiraDecoder::Receive(uint8 expected, std::vector<uint8>& output,
	bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	for (;;) {
		uint8 header[5];
		if (!Transfer(header, 5, false, deadline)) return B_IO_ERROR;
		uint32 size = ReadLE(header + 1, 4);
		if (size > 1024 * 1024) {
			fError = "Invalid local decoder message size";
			return B_BAD_DATA;
		}
		output.resize(size);
		if (size && !Transfer(&output[0], size, false, deadline)) return B_IO_ERROR;
		if (header[0] == expected) return B_OK;
		if (header[0] == 'E') {
			fError.assign(output.begin(), output.end());
			return B_ERROR;
		}
		if (header[0] == 'N') {
			fprintf(stderr, "roneseg: %.*s\n", (int)size, size ? (char*)&output[0] : "");
			continue;
		}
		if (header[0] != 'C' || expected != 'R' || size < 8 || fTuner == NULL) {
			fError = "Invalid local decoder response";
			return B_BAD_DATA;
		}
		uint8 type = output[0], request = output[1];
		uint16 value = ReadLE(&output[2], 2), index = ReadLE(&output[4], 2);
		uint16 length = ReadLE(&output[6], 2);
		bool input = type == 0xc0;
		if ((type != 0x40 && !input) || length > 64
			|| size != 8u + (input ? 0u : length)
			|| (request != 0x20 && request != 0x21 && request != 0x23
				&& request != 0x24 && request != 0x25 && request != 0x27
				&& request != 0x2b && request != 0x2c)) {
			fError = "Invalid decoder USB request";
			return B_BAD_DATA;
		}
		uint8 reply[68] = {};
		if (!input && length) memcpy(reply + 4, &output[8], length);
		ssize_t got = fTuner->DecoderControl(type, request, value, index,
			length, reply + 4);
		for (int i = 0; i < 4; i++) reply[i] = (uint32)got >> (8 * i);
		if (Send('C', reply, 4 + (input && got > 0 ? got : 0)) != B_OK)
			return B_IO_ERROR;
	}
}

status_t LeiraDecoder::Initialize()
{
	std::string launcher = OneSegDataFile("decoder/run");
	BPath path(launcher.c_str());
	if (launcher.empty() || access(path.Path(), X_OK) != 0) {
		fError = "Local tuner decoder is not installed (roneseg/decoder/run)";
		return B_ENTRY_NOT_FOUND;
	}
	int sockets[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return B_ERROR;
	fPid = fork();
	if (fPid == 0) {
		close(sockets[0]);
		dup2(sockets[1], STDIN_FILENO);
		dup2(sockets[1], STDOUT_FILENO);
		close(sockets[1]);
		if (fReplay.empty())
			execl(path.Path(), path.Path(), (char*)NULL);
		else
			execl(path.Path(), path.Path(), "--replay", fReplay.c_str(), (char*)NULL);
		_exit(127);
	}
	close(sockets[1]);
	fSocket = sockets[0];
	fcntl(fSocket, F_SETFD, FD_CLOEXEC);
	fcntl(fSocket, F_SETFL, O_NONBLOCK);
	if (fPid < 0) return B_ERROR;
	std::vector<uint8> output;
	fClean = false;
	status_t status = Receive('R', output, 240000000);
	fInitialized = fClean = status == B_OK;
	return status;
}

status_t LeiraDecoder::Reset()
{
	std::vector<uint8> output;
	fClean = false;
	status_t status = Send('R', NULL, 0) == B_OK ? Receive('R', output, 3000000) : B_IO_ERROR;
	fClean = status == B_OK;
	return status;
}

status_t LeiraDecoder::Decode(const uint8* data, size_t size, std::vector<uint8>& output)
{
	fClean = false;
	status_t status = Send('D', data, size) == B_OK ? Receive('T', output, 120000000) : B_IO_ERROR;
	fClean = status == B_OK;
	return status;
}
