#include "TunerDiagnostic.h"

#include <OS.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ChannelTable.h"
#include "UsbTuner.h"

namespace {

const uint8 kEeprom = 0x50;
const uint8 kDemod = 0x6E;
// Written by the bring-up table's neighbours but never by it; reads back what
// was written (AGENTS.md, "Register read/write behaviour, measured").
const uint8 kScratchRegister = 0x31;

} // namespace


TunerDiagnostic::TunerDiagnostic(UsbTuner* tuner, LineFunction output,
	void* cookie)
	:
	fTuner(tuner),
	fOutput(output),
	fCookie(cookie)
{
}


void
TunerDiagnostic::Emit(const char* format, ...)
{
	char line[512];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	if (fOutput != NULL)
		fOutput(line, fCookie);
}


bool
TunerDiagnostic::Run(const std::vector<int32>& channels, volatile bool* cancel)
{
	// 1. Firmware and bring-up.
	Emit("[1] firmware upload and bring-up");
	status_t status = fTuner->Open();
	if (status != B_OK) {
		Emit("    FAIL: %s", fTuner->LastError().empty()
			? strerror(status) : fTuner->LastError().c_str());
		Emit("    the module did not come up; nothing further can be tested");
		return false;
	}
	Emit("    OK: %s", fTuner->Description().c_str());

	// 2. The register bus. The EEPROM holds the identity the device enumerates
	// with, so reading it back proves host -> USB -> FX2 -> I2C -> chip.
	Emit("[2] register bus (I2C EEPROM at 0x50)");
	uint8 eeprom[8];
	bool busOk = fTuner->ReadRegisters(kEeprom, 0x00, eeprom, sizeof(eeprom));
	if (!busOk) {
		Emit("    FAIL: the EEPROM did not answer");
		return false;
	}
	Emit("    read %02x %02x %02x %02x %02x %02x %02x %02x", eeprom[0],
		eeprom[1], eeprom[2], eeprom[3], eeprom[4], eeprom[5], eeprom[6],
		eeprom[7]);
	busOk = eeprom[1] == 0x4C && eeprom[2] == 0x05 && eeprom[3] == 0x79
		&& eeprom[4] == 0x02;
	Emit(busOk ? "    OK: VID 054c PID 0279 as enumerated"
		: "    FAIL: expected 4c 05 79 02 (VID 054c, PID 0279)");
	if (!busOk)
		return false;

	// 3. The demodulator: write two patterns to a scratch register, read each
	// back, then restore the original value.
	Emit("[3] demodulator write/read-back (0x6E register 0x%02x)",
		kScratchRegister);
	uint8 original = 0;
	uint8 readBack[2] = { 0, 0 };
	const uint8 patterns[2] = { 0xAA, 0x55 };
	bool demodOk = fTuner->ReadRegisters(kDemod, kScratchRegister, &original, 1);
	for (int i = 0; demodOk && i < 2; i++) {
		demodOk = fTuner->PokeRegister(kDemod, kScratchRegister, patterns[i])
			&& fTuner->ReadRegisters(kDemod, kScratchRegister, &readBack[i], 1)
			&& readBack[i] == patterns[i];
	}
	fTuner->PokeRegister(kDemod, kScratchRegister, original);
	if (demodOk) {
		Emit("    OK: wrote aa/55, read %02x/%02x, restored %02x", readBack[0],
			readBack[1], original);
	} else {
		Emit("    FAIL: wrote aa/55, read %02x/%02x", readBack[0], readBack[1]);
		return false;
	}

	// 4. Every channel: the demodulator's own verdict and the strength the
	// AGC reports. A channel that is on air reads clearly above the rest
	// even when it is too weak to lock.
	Emit("[4] channels: lock and signal strength");
	Emit("    UHF  MHz      strength   lock");
	int32 lockedChannel = -1;
	int locked = 0;
	int tuneFailures = 0;
	float strongest = -100;
	int32 strongestChannel = -1;
	for (size_t i = 0; i < channels.size(); i++) {
		if (cancel != NULL && *cancel) {
			Emit("    cancelled");
			break;
		}
		uint64 hz = ChannelTable::FrequencyFor(channels[i]);
		if (fTuner->Tune(hz) != B_OK) {
			tuneFailures++;
			Emit("    %2d  %7.3f  tuning FAILED", (int)channels[i], hz / 1e6);
			continue;
		}
		UsbTuner::LockState lock = fTuner->WaitForLock(1500000);
		float dB = 0;
		bool measured = fTuner->MeasureSignal(&dB);
		if (measured && dB > strongest) {
			strongest = dB;
			strongestChannel = channels[i];
		}
		if (lock == UsbTuner::kLocked) {
			locked++;
			if (lockedChannel < 0)
				lockedChannel = channels[i];
		}
		char strength[16] = "     --";
		if (measured)
			snprintf(strength, sizeof(strength), "%+6.1f dB", dB);
		Emit("    %2d  %7.3f  %s   %s", (int)channels[i], hz / 1e6, strength,
			lock == UsbTuner::kLocked ? "LOCKED"
				: lock == UsbTuner::kNoSignal ? "-" : "read error");
	}

	// 5. The data endpoint, only on a locked channel: a bulk read with nothing
	// to deliver never completes and blocks the control requests after it.
	ssize_t bytes = 0;
	bool sync = false;
	if (lockedChannel >= 0) {
		Emit("[5] data endpoint (UHF %d)", (int)lockedChannel);
		fTuner->HasSignal(ChannelTable::FrequencyFor(lockedChannel), 1500000);
		UsbTuner::Diagnostic d = fTuner->LastDiagnostic();
		bytes = d.bytes;
		sync = d.sync;
		Emit("    %ld bytes%s", (long)bytes, bytes <= 0 ? ""
			: sync ? ", TS sync found" : ", no TS sync");
	} else
		Emit("[5] data endpoint: skipped, no channel locked");

	Emit("[summary]");
	Emit("    USB, firmware, register bus and demodulator: OK");
	if (tuneFailures > 0)
		Emit("    tuning failed on %d channel(s)", tuneFailures);
	if (strongestChannel >= 0) {
		Emit("    strongest: UHF %d at %+.1f dB (about 7 dB is needed to lock;",
			(int)strongestChannel, strongest);
		Emit("      0-3 dB is the machine's own noise)");
	}
	Emit("    locked: %d channel(s)", locked);
	Emit("    data endpoint: %s", lockedChannel < 0 ? "not tried"
		: bytes > 0 ? (sync ? "transport stream" : "bytes without TS sync")
		: "silent");
	return true;
}
