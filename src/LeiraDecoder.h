#ifndef RONESEG_LEIRA_DECODER_H
#define RONESEG_LEIRA_DECODER_H

#include <OS.h>
#include <string>
#include <vector>

class UsbTuner;

// Local child process: recovered link protocol and native OpenSSL, isolated
// from the GUI. No Windows code or network transport. One caller at a time.
class LeiraDecoder {
public:
	explicit LeiraDecoder(UsbTuner* tuner, const std::string& replay = std::string());
	~LeiraDecoder();
	status_t Initialize();
	status_t Reset();
	status_t Decode(const uint8* data, size_t size, std::vector<uint8>& output);
	void Cancel();
	bool Resume();
	const std::string& Error() const { return fError; }

private:
	status_t Send(uint8 kind, const void* data, size_t size);
	status_t Receive(uint8 expected, std::vector<uint8>& output,
		bigtime_t timeout);
	bool Transfer(void* data, size_t size, bool writing, bigtime_t deadline);
	void Close();
	UsbTuner* fTuner;
	int fSocket;
	int fPid;
	int32 fCancelled;
	std::string fError;
	std::string fReplay;
	bool fInitialized;
	bool fClean;
};
#endif
