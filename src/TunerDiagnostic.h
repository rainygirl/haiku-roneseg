#ifndef RONESEG_TUNER_DIAGNOSTIC_H
#define RONESEG_TUNER_DIAGNOSTIC_H

#include <SupportDefs.h>

#include <string>
#include <vector>

class UsbTuner;

// Checks the internal One-Seg module stage by stage, without needing an
// ISDB-T broadcast - there is none in Korea, where this is developed.
//
//   1. firmware upload and bring-up (UsbTuner::Open)
//   2. the register bus: the module's I2C EEPROM must read back the
//      VID/PID the device enumerates with
//   3. the demodulator: a scratch register must read back what was written
//   4. RF response: demodulator registers read per UHF channel; registers
//      that change with the channel, beyond those that drift on their own,
//      are the evidence that tuning reaches the chip
//   5. the data endpoint: bytes and TS sync per channel
//
// Stages 1-3 are a pass/fail answer. Stage 4 is a table to read, not a
// verdict: which demodulator register reports signal is not identified, so
// the report shows what moved and lets a channel known to be on air (even a
// Korean ATSC one, which shares the 6 MHz UHF raster) stand out.
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
	bool ReadDemod(uint8* out);		// registers 0x00-0x7F

	UsbTuner*		fTuner;
	LineFunction	fOutput;
	void*			fCookie;
};

#endif
