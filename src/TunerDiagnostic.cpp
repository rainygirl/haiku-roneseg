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
const int kDemodRegisters = 128;
// Baseline reads with nothing tuned, to learn which registers drift on their
// own. Without it every reading looks like a result.
const int kBaselineReads = 3;
// Columns in the per-channel table beyond 0x14.
const size_t kMaxColumns = 8;

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
TunerDiagnostic::ReadDemod(uint8* out)
{
	return fTuner->ReadRegisters(kDemod, 0x00, out, 64)
		&& fTuner->ReadRegisters(kDemod, 0x40, out + 64, 64);
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

	// 4 and 5. Baseline, then every channel.
	Emit("[4] demodulator registers per channel");
	bool drifts[kDemodRegisters];
	memset(drifts, 0, sizeof(drifts));
	uint8 baseline[kDemodRegisters];
	if (!ReadDemod(baseline)) {
		Emit("    FAIL: could not read the demodulator registers");
		return true;
	}
	for (int n = 1; n < kBaselineReads; n++) {
		snooze(200000);
		uint8 again[kDemodRegisters];
		if (!ReadDemod(again))
			break;
		for (int r = 0; r < kDemodRegisters; r++) {
			if (again[r] != baseline[r])
				drifts[r] = true;
		}
	}
	// Tuning writes these itself; their change proves the write, not RF.
	drifts[fTuner->FrequencyRegister() & 0x7F] = true;
	drifts[fTuner->FrequencyRegisterLow() & 0x7F] = true;
	drifts[0x42] = true;

	std::string drifting;
	for (int r = 0; r < kDemodRegisters; r++) {
		if (drifts[r] && r != (fTuner->FrequencyRegister() & 0x7F)
			&& r != (fTuner->FrequencyRegisterLow() & 0x7F) && r != 0x42) {
			char name[8];
			snprintf(name, sizeof(name), " %02x", r);
			drifting += name;
		}
	}
	Emit("    drifting on their own (ignored):%s",
		drifting.empty() ? " none" : drifting.c_str());

	// Two passes over every channel. A register only counts as responding to
	// the channel if both passes read the same value on every channel and the
	// channels disagree with each other. One pass is not enough: on hardware,
	// 0x12/0x13 sat still through the baseline and then flipped between ff
	// and 00 every few seconds, which a single pass reported as "changes with
	// the channel" (UHF 13 read ff, 00 and 00 on three visits).
	struct Row {
		int32	channel;
		uint64	hz;
		uint8	regs[2][kDemodRegisters];
		bool	regsOk[2];
		bool	tuned[2];
	};
	std::vector<Row> rows(channels.size());
	size_t visited = 0;
	for (int pass = 0; pass < 2; pass++) {
		Emit("    pass %d of 2", pass + 1);
		for (size_t i = 0; i < channels.size(); i++) {
			if (cancel != NULL && *cancel) {
				Emit("    cancelled");
				pass = 2;
				break;
			}
			Row& row = rows[i];
			row.channel = channels[i];
			row.hz = ChannelTable::FrequencyFor(channels[i]);
			row.tuned[pass] = fTuner->Tune(row.hz) == B_OK;
			snooze(400000);				// let the front end and AGC settle
			row.regsOk[pass] = ReadDemod(row.regs[pass]);
			if (pass == 1)
				visited = i + 1;
			if (!row.tuned[pass] || !row.regsOk[pass]) {
				Emit("    UHF %2d: %s", (int)row.channel, !row.tuned[pass]
					? "tuning write FAILED" : "registers unreadable");
			}
		}
	}
	rows.resize(visited);

	// Classify every register that does not drift during the baseline.
	std::vector<int> responding;
	std::vector<int> unstable;
	for (int r = 0; r < kDemodRegisters; r++) {
		if (drifts[r])
			continue;
		bool repeatable = true;
		bool differs = false;
		bool seen = false;
		uint8 first = 0;
		for (size_t i = 0; i < rows.size(); i++) {
			const Row& row = rows[i];
			if (!row.regsOk[0] || !row.regsOk[1])
				continue;
			if (row.regs[0][r] != row.regs[1][r])
				repeatable = false;
			if (!seen) {
				first = row.regs[0][r];
				seen = true;
			} else if (row.regs[0][r] != first)
				differs = true;
		}
		if (!repeatable)
			unstable.push_back(r);
		else if (differs)
			responding.push_back(r);
	}

	// Registers that hold the frequency word itself after the latch. On
	// hardware, with the word in 0x64/0x67 and latch 0x42 = 0x10, register
	// 0x10 read back exactly the high byte of V = 7 x f_MHz on every channel
	// in both passes; with any other layout no register did. That is the chip
	// taking the word in - the strongest evidence available for the layout
	// without a transmitter.
	std::vector<int> latchedHigh;
	std::vector<int> latchedLow;
	// V's high byte only changes every 256/42 channels or so; over a narrow
	// range it is constant and the high-byte test proves nothing either way.
	bool highVaries = rows.size() > 1
		&& ((int)(7.0 * rows.front().hz / 1000000.0 + 0.5) >> 8)
			!= ((int)(7.0 * rows.back().hz / 1000000.0 + 0.5) >> 8);
	for (int r = 0; r < kDemodRegisters; r++) {
		if (r == (fTuner->FrequencyRegister() & 0x7F)
			|| r == (fTuner->FrequencyRegisterLow() & 0x7F) || r == 0x42)
			continue;
		bool high = !rows.empty();
		bool low = !rows.empty();
		for (size_t i = 0; i < rows.size(); i++) {
			const Row& row = rows[i];
			int v = (int)(7.0 * row.hz / 1000000.0 + 0.5);
			for (int pass = 0; pass < 2; pass++) {
				if (!row.regsOk[pass]) {
					high = low = false;
					continue;
				}
				if (row.regs[pass][r] != (uint8)(v >> 8))
					high = false;
				if (row.regs[pass][r] != (uint8)(v & 0xFF))
					low = false;
			}
		}
		// A constant register can equal a constant high byte by accident, so
		// it only counts when the high byte itself varies across the scan.
		if (high && highVaries)
			latchedHigh.push_back(r);
		if (low && rows.size() > 1)
			latchedLow.push_back(r);
	}

	// A register that holds the written word tracks the channel trivially;
	// it is not evidence of RF, so keep it out of that list.
	for (size_t i = 0; i < responding.size();) {
		bool latched = false;
		for (size_t j = 0; j < latchedHigh.size(); j++)
			latched = latched || latchedHigh[j] == responding[i];
		for (size_t j = 0; j < latchedLow.size(); j++)
			latched = latched || latchedLow[j] == responding[i];
		if (latched)
			responding.erase(responding.begin() + i);
		else
			i++;
	}

	// The table: 0x14 (the register seen moving on its own), then the
	// responding registers, each as "pass1/pass2".
	std::vector<int> columns;
	columns.push_back(0x14);
	for (size_t i = 0; i < responding.size() && columns.size() <= kMaxColumns;
			i++) {
		if (responding[i] != 0x14)
			columns.push_back(responding[i]);
	}
	std::string header = "    UHF  MHz     ";
	for (size_t c = 0; c < columns.size(); c++) {
		char name[16];
		snprintf(name, sizeof(name), "   %02x  ", columns[c]);
		header += name;
	}
	Emit("%s", header.c_str());
	int tuneFailures = 0;
	for (size_t i = 0; i < rows.size(); i++) {
		const Row& row = rows[i];
		if (!row.tuned[0] || !row.tuned[1])
			tuneFailures++;
		char line[256];
		int at = snprintf(line, sizeof(line), "    %2d  %7.3f",
			(int)row.channel, row.hz / 1e6);
		for (size_t c = 0; c < columns.size() && at < (int)sizeof(line) - 9;
				c++) {
			if (row.regsOk[0] && row.regsOk[1]) {
				at += snprintf(line + at, sizeof(line) - at, "  %02x/%02x",
					row.regs[0][columns[c]], row.regs[1][columns[c]]);
			} else
				at += snprintf(line + at, sizeof(line) - at, "  --/--");
		}
		Emit("%s", line);
	}

	// 5. The data endpoint, once, last. A bulk read that times out stays
	// queued in the USB stack and blocks the control requests after it, so
	// it must not come before any register access.
	Emit("[5] data endpoint (one read, UHF %d)", rows.empty() ? 0
		: (int)rows.front().channel);
	ssize_t bytes = 0;
	bool sync = false;
	if (!rows.empty()) {
		fTuner->HasSignal(rows.front().hz, 1500000);
		UsbTuner::Diagnostic d = fTuner->LastDiagnostic();
		bytes = d.bytes;
		sync = d.sync;
	}
	if (bytes > 0) {
		Emit("    %ld bytes%s", (long)bytes, sync ? ", TS sync found"
			: ", no TS sync");
	} else {
		Emit("    nothing arrived (expected without an ISDB-T signal:");
		Emit("      the demodulator only streams once it locks)");
	}

	Emit("[summary]");
	Emit("    USB, firmware, register bus and demodulator: OK");
	if (tuneFailures > 0)
		Emit("    tuning writes failed on %d of %d channels", tuneFailures,
			(int)rows.size());
	std::string list;
	if (!unstable.empty())
		Emit("    slow drift, ignored (differed between passes):");
	for (size_t i = 0; i < unstable.size(); i++) {
		char name[8];
		snprintf(name, sizeof(name), " %02x", unstable[i]);
		list += name;
		if ((i + 1) % 12 == 0 || i + 1 == unstable.size()) {
			Emit("     %s", list.c_str());
			list.clear();
		}
	}
	if (latchedHigh.empty() && latchedLow.empty() && !highVaries) {
		Emit("    frequency word: cannot tell from this channel range (the");
		Emit("      high byte of V does not change); scan 13-52 instead.");
	} else if (latchedHigh.empty() && latchedLow.empty()) {
		Emit("    frequency word: NOT taken in - no register holds the word");
		Emit("      after the latch. This layout is probably wrong;");
		Emit("      try the default, 64/67 with latch 10, in the settings.");
	} else {
		std::string where;
		for (size_t i = 0; i < latchedHigh.size(); i++) {
			char name[24];
			snprintf(name, sizeof(name), " %02x (high byte)", latchedHigh[i]);
			where += name;
		}
		for (size_t i = 0; i < latchedLow.size(); i++) {
			char name[24];
			snprintf(name, sizeof(name), " %02x (low byte)", latchedLow[i]);
			where += name;
		}
		Emit("    frequency word: taken in by the demodulator -%s",
			where.c_str());
	}
	if (responding.empty()) {
		Emit("    RF response: none detected - no demodulator register tracks");
		Emit("      the channel repeatably. Either nothing receivable is on");
		Emit("      air, or tuning does not reach the RF front end; only an");
		Emit("      ISDB-T signal can tell which.");
	} else {
		list.clear();
		for (size_t i = 0; i < responding.size(); i++) {
			char name[8];
			snprintf(name, sizeof(name), " %02x", responding[i]);
			list += name;
		}
		Emit("    RF response: registers that track the channel repeatably:%s",
			list.c_str());
	}
	Emit("    data endpoint: %s", bytes > 0 ? (sync ? "transport stream"
		: "bytes without TS sync") : "silent");
	return true;
}
