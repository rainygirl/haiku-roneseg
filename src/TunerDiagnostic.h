#ifndef RONESEG_TUNER_DIAGNOSTIC_H
#define RONESEG_TUNER_DIAGNOSTIC_H

#include <SupportDefs.h>

#include <string>
#include <vector>

class UsbTuner;

// Checks the internal One-Seg module stage by stage:
//
//   1. firmware upload and bring-up (UsbTuner::Open)
//   2. the register bus: the module's I2C EEPROM must read back the
//      VID/PID the device enumerates with
//   3. the demodulator: a scratch register must read back what was written
//   4. every UHF channel: the demodulator's lock verdict and the signal
//      strength its AGC reports (UsbTuner::MeasureSignal)
//   5. the data endpoint, on the first channel that locked
//
// Stages 1-3 are a pass/fail answer. Stage 4 is a table to read: a channel
// that is on air stands above the rest even when it is too weak to lock.
//
// Transfers are budgeted: registers are read 64 at a time, and every request
// goes through UsbTuner's timed path. An unanswered request parks a thread in
// the USB stack; enough of them wedge the module until a reboot.
class TunerDiagnostic {
public:
	typedef void (*LineFunction)(const std::string& line, void* cookie);

	TunerDiagnostic(UsbTuner* tuner, LineFunction output, void* cookie);

	// Runs every stage over the given UHF channels. cancel is polled between
	// channels. Returns true when stages 1-3 passed.
	bool Run(const std::vector<int32>& channels, volatile bool* cancel);

private:
	void Emit(const char* format, ...);

	UsbTuner*		fTuner;
	LineFunction	fOutput;
	void*			fCookie;
};

#endif
