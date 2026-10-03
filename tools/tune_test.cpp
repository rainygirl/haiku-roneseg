// Bring the module up with UsbTuner, tune each given UHF channel, report the
// demodulator's lock verdict and, on a lock, capture the transport stream.
//
// Build:  setarch x86 g++ -O2 -Isrc -o tune_test tools/tune_test.cpp \
//             src/UsbTuner.cpp src/LeiraDecoder.cpp -ldevice -lbe
// Run:    ./tune_test 22,26,28 [seconds-to-capture] [out.ts]

#include "UsbTuner.h"

#include <OS.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>


static volatile bool sReading = false;
static volatile size_t sReadBytes = 0;


static status_t
ReaderThread(void* cookie)
{
	UsbTuner* tuner = (UsbTuner*)cookie;
	static uint8 buffer[16384];
	while (sReading) {
		ssize_t got = tuner->ReadRaw(buffer, 416 * 32);
		if (got > 0)
			sReadBytes += got;
	}
	return B_OK;
}


static uint64
ChannelHz(int channel)
{
	return (uint64)((473.0 + 1.0 / 7.0 + 6.0 * (channel - 13)) * 1000000.0
		+ 0.5);
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s CH[,CH...] [seconds] [out.ts]\n", argv[0]);
		return 2;
	}
	int seconds = argc > 2 ? atoi(argv[2]) : 3;
	const char* outPath = argc > 3 ? argv[3] : NULL;
	setvbuf(stdout, NULL, _IOLBF, 0);

	UsbTuner tuner;
	bigtime_t start = system_time();
	status_t status = tuner.Open();
	if (status != B_OK) {
		printf("open failed: %s (%s)\n", strerror(status),
			tuner.LastError().c_str());
		return 1;
	}
	printf("open: %.1f s\n", (system_time() - start) / 1e6);

	thread_id reader = -1;
	if (getenv("STREAM") != NULL) {
		sReading = true;
		reader = spawn_thread(ReaderThread, "reader", B_NORMAL_PRIORITY,
			&tuner);
		resume_thread(reader);
	}

	// UNTIL=N: go round the list up to N times and stop on the first lock,
	// leaving the tuner there - for a signal that comes and goes.
	int rounds = getenv("UNTIL") != NULL ? atoi(getenv("UNTIL")) : 1;
	std::vector<int> channels;
	char* list = strdup(argv[1]);
	for (char* token = strtok(list, ","); token != NULL;
			token = strtok(NULL, ","))
		channels.push_back(atoi(token));
	bool stopOnLock = getenv("UNTIL") != NULL;
	bool locked = false;
	for (size_t n = 0; n < channels.size() * rounds && !locked; n++) {
		int channel = channels[n % channels.size()];
		start = system_time();
		status = tuner.Tune(ChannelHz(channel));
		if (status != B_OK) {
			printf("UHF %d: tune failed (%s)\n", channel,
				tuner.LastError().c_str());
			continue;
		}
		UsbTuner::LockState lock = tuner.WaitForLock(1500000);
		if (stopOnLock && lock == UsbTuner::kLocked)
			locked = true;
		uint8 regs[4] = { 0 };
		tuner.ReadRegisters(0x6e, 0x00, regs, 4);
		printf("UHF %d: %s  (00=%02x 02=%02x, %.2f s)\n", channel,
			lock == UsbTuner::kLocked ? "LOCKED"
				: lock == UsbTuner::kNoSignal ? "no signal" : "read error",
			regs[0], regs[2], (system_time() - start) / 1e6);
		if (getenv("AGC") != NULL) {
			int sum = 0, n = 0;
			uint8 r[16];
			for (int i = 0; i < 10; i++) {
				if (tuner.ReadRegisters(0x6e, 0x10, r, 16)) {
					sum += r[1];
					n++;
				}
				snooze(30000);
			}
			printf("  AGC 0x11 mean %.1f\n", n ? (double)sum / n : -1.0);
		}
		if (getenv("CN") != NULL) {
			// DtvCore's C/N measurement: set 0x31 bit 7, wait for 0x08 bit 7.
			for (int pass = 0; pass < 3; pass++) {
				uint8 v31 = 0, r[3] = { 0 };
				tuner.ReadRegisters(0x6e, 0x31, &v31, 1);
				tuner.PokeRegister(0x6e, 0x31, v31 | 0x80);
				bool ready = false;
				for (int i = 0; i < 20 && !ready; i++) {
					snooze(50000);
					tuner.ReadRegisters(0x6e, 0x08, r, 3);
					ready = (r[0] & 0x80) != 0;
				}
				uint8 level = 0;
				tuner.ReadRegisters(0x6e, 0x15, &level, 1);
				tuner.PokeRegister(0x6e, 0x31, v31 & 0x7f);
				printf("  C/N %s %u  (08-0a %02x %02x %02x, 15=%02x)\n",
					ready ? "ready" : "NOT ready",
					(unsigned)((((r[0] & 0x7f) << 8) | r[1]) * 30), r[0], r[1],
					r[2], level);
			}
		}
		if (getenv("RF") != NULL) {
			// The tuner's live registers, read MaxLinear-style: write
			// (0xFB, reg) then read one byte, repeater open.
			printf("  tuner:");
			for (int pass = 0; pass < 3; pass++) {
				tuner.PokeRegister(0x6e, 0x42, 0x10);
				const uint8 regs[] = { 0x29, 0x2a, 0x2b, 0x2c, 0x30, 0x31, 0x34 };
				for (size_t i = 0; i < sizeof(regs); i++) {
					tuner.PokeRegister(0x63, 0xfb, regs[i]);
					uint8 v = 0;
					tuner.ReadRegisters(0x63, 0xfb, &v, 1);
					printf(" %02x", v);
				}
				tuner.PokeRegister(0x6e, 0x42, 0x00);
				printf(" |");
				snooze(200000);
			}
			printf("\n");
		}
		if (getenv("WATCH") != NULL) {
			for (int i = 0; i < 25; i++) {
				uint8 r[32];
				snooze(200000);
				if (tuner.ReadRegisters(0x6e, 0x00, r, 32))
					printf("  %4.1f s: 00=%02x 01=%02x 02=%02x 03=%02x 08-0a=%02x%02x%02x 11=%02x 14=%02x 15=%02x 18=%02x%02x\n",
						(system_time() - start) / 1e6, r[0], r[1], r[2], r[3],
						r[8], r[9], r[10], r[0x11], r[0x14], r[0x15], r[0x18], r[0x19]);
			}
		}
		if (getenv("DUMP") != NULL) {
			uint8 block[48];
			snooze(500000);
			if (tuner.ReadRegisters(0x6e, 0x00, block, 48)) {
				printf("  ");
				for (int i = 0; i < 48; i++)
					printf("%02x%s", block[i], (i & 15) == 15 ? " | " : " ");
				printf("\n");
			}
		}
		if (lock != UsbTuner::kLocked)
			continue;

		FILE* out = outPath != NULL ? fopen(outPath, "wb") : NULL;
		static uint8 buffer[65536];
		size_t total = 0, packets = 0, synced = 0;
		bigtime_t until = system_time() + seconds * 1000000LL;
		while (system_time() < until) {
			ssize_t got = tuner.ReadRaw(buffer, 416 * 32);
			if (got <= 0)
				continue;
			total += got;
			for (ssize_t i = 0; i + 188 <= got; i += 188) {
				packets++;
				if (buffer[i] == 0x47)
					synced++;
			}
			if (out != NULL)
				fwrite(buffer, 1, got, out);
		}
		if (out != NULL)
			fclose(out);
		if (seconds > 0)
			printf("  %zu bytes in %d s (%.0f kbit/s), %zu/%zu packets start 0x47\n",
				total, seconds, total * 8.0 / 1000.0 / seconds, synced, packets);
		outPath = NULL;		// capture only the first locked channel
	}
	free(list);
	if (reader >= 0) {
		sReading = false;
		status_t ignored;
		wait_for_thread(reader, &ignored);
		printf("background reader: %zu bytes\n", (size_t)sReadBytes);
	}
	return stopOnLock && !locked ? 3 : 0;
}
