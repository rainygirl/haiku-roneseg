// Bring the tuner fully up - demodulator AND RF front end - then walk the UHF
// channels and watch the whole demodulator register space for a reaction.
//
//   * Demodulator init: a reset/latch pulse on 0x42 and a 35-register
//     configuration table to sub-device 0x6E. Notably it leaves registers
//     0x32-0x35 at zero - the natural place for a per-channel frequency word.
//   * RF front-end init: a 16-entry table to sub-device 0x6C and the tuner IC
//     program to sub-device 0x60.
//   * The stored frequency is V = 7 x f_MHz (3312 for UHF 13, +42 per channel).
//   * The firmware relays register access, so every byte here is exactly what
//     goes on the I2C bus.
//
// (a) It replays both init sequences so the demodulator is actually running -
//     without that its status registers are dead and no reading means anything.
// (b) It reads the demodulator register block directly (bRequest 0x21 with
//     wValue = count, the read that is known to work here - NOT the 0x21+0x25
//     two-step, which fails and parks driver threads) and, rather than trusting
//     one hard-coded lock register, samples the whole block twice for a
//     baseline, then per channel reports any register that moves outside its
//     own jitter. That is how the real lock/quality register reveals itself.
//
// A register that comes alive when a channel is tuned is the answer. In Korea
// there is no ISDB-T signal, so the honest expected result is that the block
// stays put - but now that is a meaningful negative, because the demodulator is
// genuinely running.
//
// Every transfer is timed on its own thread: an unanswered control transfer
// parks a driver thread that kill(1) cannot reclaim. Run once per fresh boot.
//
// Build:  setarch x86 g++ -O2 -o scan_real scan_real.cpp -ldevice
// Run:    ./scan_real                    full UHF 13-62 scan
//         ./scan_real --ch 20            one channel, verbose
//         ./scan_real --target 6e:32     where to write V (default demod 0x32)
//         ./scan_real --dump             just init + dump the demod block

#include <USBKit.h>
#include <OS.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8 kIn = 0xC0;
static const uint8 kOut = 0x40;
static const uint8 kRegisterWrite = 0x20;
static const uint8 kRegisterRead = 0x21;

static const uint8 kDemod = 0x6E;
static const uint8 kTuner = 0x60;
static const uint8 kFront = 0x6C;

static const bigtime_t kTimeout = 1500000;
static const bigtime_t kSettle = 5000;


// ---- demodulator init: reset/latch pulse, then a 35-register config --------
static const uint8 kDemodLatch[][2] = {
	{ 0x42, 0x01 }, { 0x42, 0x00 }, { 0x41, 0x01 },
};
static const uint8 kDemodInit[][2] = {
	{ 0x32, 0x00 }, { 0x33, 0x00 }, { 0x34, 0x00 }, { 0x35, 0x00 }, { 0x40, 0x00 },
	{ 0x43, 0x0d }, { 0x44, 0x22 }, { 0x46, 0x01 }, { 0x47, 0x14 }, { 0x48, 0x00 },
	{ 0x49, 0x01 }, { 0x4b, 0x16 }, { 0x4c, 0x80 }, { 0x4d, 0x8f }, { 0x4e, 0x82 },
	{ 0x4f, 0x08 }, { 0x50, 0x24 }, { 0x51, 0x8b }, { 0x52, 0x01 }, { 0x53, 0x01 },
	{ 0x56, 0x84 }, { 0x57, 0x20 }, { 0x59, 0xff }, { 0x5a, 0x69 }, { 0x5b, 0x12 },
	{ 0x5d, 0x40 }, { 0x5e, 0x90 }, { 0x5f, 0xff }, { 0x60, 0x00 }, { 0x62, 0x20 },
	{ 0x64, 0x00 }, { 0x65, 0x10 }, { 0x66, 0x00 }, { 0x67, 0x00 }, { 0x68, 0x83 },
};

// ---- RF front-end init: sub-device 0x6C table and sub-device 0x60 tuner ----
static const uint8 kFront6C[][2] = {
	{ 0x00, 0x20 }, { 0x4a, 0x2a }, { 0x84, 0x07 }, { 0x1b, 0x73 }, { 0x1c, 0x95 },
	{ 0x0b, 0x00 }, { 0x0c, 0x31 }, { 0x0d, 0x35 }, { 0x98, 0x80 }, { 0xdf, 0x01 },
	{ 0x05, 0x18 }, { 0x08, 0x32 }, { 0x61, 0x80 }, { 0x3c, 0x10 }, { 0x3d, 0x80 },
	{ 0xf2, 0x01 }, { 0xf4, 0x0a }, { 0xe7, 0x02 },
};
static const uint8 kTuner60[][2] = {
	{ 0xa0, 0x54 }, { 0xa1, 0x02 }, { 0xa2, 0x67 }, { 0xa3, 0x00 }, { 0xa4, 0x06 },
	{ 0xa5, 0x3b }, { 0xa6, 0x00 }, { 0xa7, 0x56 }, { 0xa8, 0x00 }, { 0xa9, 0x5b },
	{ 0xaa, 0x02 }, { 0xab, 0x6e }, { 0xac, 0x02 }, { 0xad, 0x0b }, { 0xae, 0x01 },
	{ 0xaf, 0x42 }, { 0xb0, 0x01 }, { 0xb1, 0x0b }, { 0xb2, 0x00 }, { 0xb5, 0x00 },
};


struct Job {
	BUSBDevice*	device;
	uint8		requestType;
	uint8		request;
	uint16		value;
	uint16		index;
	uint16		length;
	uint8*		buffer;
	ssize_t		result;
	sem_id		done;
};


static status_t
Entry(void* cookie)
{
	Job* job = (Job*)cookie;
	job->result = job->device->ControlTransfer(job->requestType, job->request,
		job->value, job->index, job->length, job->buffer);
	release_sem(job->done);
	return B_OK;
}


static ssize_t
Transfer(BUSBDevice& device, uint8 requestType, uint8 request, uint16 value,
	uint16 index, uint16 length, uint8* buffer)
{
	uint8 scratch = 0;
	Job job;
	job.device = &device;
	job.requestType = requestType;
	job.request = request;
	job.value = value;
	job.index = index;
	job.length = length;
	job.buffer = buffer != NULL ? buffer : &scratch;
	job.result = 0;
	job.done = create_sem(0, "scan");

	thread_id thread = spawn_thread(Entry, "scan", B_NORMAL_PRIORITY, &job);
	if (thread < 0) {
		delete_sem(job.done);
		return -1;
	}
	resume_thread(thread);

	status_t waited = acquire_sem_etc(job.done, 1, B_RELATIVE_TIMEOUT, kTimeout);
	delete_sem(job.done);
	snooze(kSettle);
	return waited == B_OK ? job.result : -1;
}


static bool
Write(BUSBDevice& device, uint8 sub, uint8 reg, uint8 value)
{
	uint8 byte = value;
	return Transfer(device, kOut, kRegisterWrite, 1,
		(uint16)((sub << 8) | reg), 1, &byte) == 1;
}


// Direct block read: bRequest 0x21 with wValue = number of registers. This is
// the read that works on this hardware.
static bool
ReadBlock(BUSBDevice& device, uint8 sub, uint8 base, uint8* out, int n)
{
	return Transfer(device, kIn, kRegisterRead, (uint16)n,
		(uint16)((sub << 8) | base), (uint16)n, out) == n;
}


static bool
WriteTable(BUSBDevice& device, uint8 sub, const uint8 table[][2], int n)
{
	for (int i = 0; i < n; i++) {
		if (!Write(device, sub, table[i][0], table[i][1]))
			return false;
	}
	return true;
}


int
main(int argc, char** argv)
{
	int oneChannel = -1;
	bool dumpOnly = false;
	uint8 targetSub = kDemod;	// where the per-channel frequency word goes
	uint8 targetReg = 0x32;		// demod 0x32-0x35 are left at zero by init

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--ch") == 0 && i + 1 < argc)
			oneChannel = (int)strtol(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "--dump") == 0)
			dumpOnly = true;
		else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
			char* colon = strchr(argv[++i], ':');
			if (colon != NULL) {
				*colon = '\0';
				targetSub = (uint8)strtol(argv[i], NULL, 16);
				targetReg = (uint8)strtol(colon + 1, NULL, 16);
			}
		}
	}

	setvbuf(stdout, NULL, _IONBF, 0);

	BUSBDevice device("/dev/bus/usb/3/2");
	if (device.InitCheck() != B_OK) {
		fprintf(stderr, "cannot open the device\n");
		return 1;
	}
	if (device.Descriptor()->device_version == 0) {
		printf("Still the boot loader - run upload_fw first.\n");
		return 1;
	}
	printf("%s ver %04x\n", device.ProductString(),
		device.Descriptor()->device_version);

	// A known-good transfer first: if this times out the device is wedged from
	// a previous run and nothing below is meaningful.
	uint8 mode = 0;
	if (Transfer(device, kIn, 0x24, 0, 0, 1, &mode) != 1) {
		printf("The device is not answering a known-good command - it is wedged."
			"\nPower-cycle it (or re-upload the firmware) and try again.\n");
		return 1;
	}
	Transfer(device, kOut, 0x23, 1, 0, 0, NULL);		// mode 1

	// (a) Full bring-up: demodulator, then RF front end.
	printf("demod init (latch + %d regs)... ",
		(int)(sizeof(kDemodInit) / 2));
	if (!WriteTable(device, kDemod, kDemodLatch,
			sizeof(kDemodLatch) / 2)
		|| !WriteTable(device, kDemod, kDemodInit, sizeof(kDemodInit) / 2)) {
		printf("a write timed out - wedged, power-cycle.\n");
		return 1;
	}
	printf("ok\nfront-end init (0x6C table + 0x60 tuner)... ");
	if (!WriteTable(device, kFront, kFront6C, sizeof(kFront6C) / 2)
		|| !WriteTable(device, kTuner, kTuner60, sizeof(kTuner60) / 2)) {
		printf("a write timed out - wedged, power-cycle.\n");
		return 1;
	}
	printf("ok\n");

	// Read the demodulator block (0x00-0x3F) twice for a baseline, so the
	// registers that move on their own can be excluded from a reaction.
	uint8 base[64], again[64];
	bool jitter[64];
	if (!ReadBlock(device, kDemod, 0x00, base, 64)) {
		printf("cannot read the demod block - stopping.\n");
		return 1;
	}
	snooze(400000);
	ReadBlock(device, kDemod, 0x00, again, 64);

	int alive = 0, moving = 0;
	for (int i = 0; i < 64; i++) {
		jitter[i] = base[i] != again[i];
		if (base[i] != 0)
			alive++;
		if (jitter[i])
			moving++;
	}

	printf("\ndemod block 0x00-0x3F after bring-up:\n");
	for (int row = 0; row < 64; row += 16) {
		printf("  %02x:", row);
		for (int col = 0; col < 16; col++)
			printf(" %02x", base[row + col]);
		printf("\n");
	}
	printf("%d non-zero register(s), %d self-moving", alive, moving);
	if (moving > 0) {
		printf(" -");
		for (int i = 0; i < 64; i++)
			if (jitter[i])
				printf(" %02x", i);
	}
	printf("\n");
	if (alive == 0)
		printf("WARNING: the whole block is zero - the demod is not running; a "
			"lock reading would be meaningless.\n");

	if (dumpOnly)
		return 0;

	printf("\nwriting V to sub 0x%02x reg 0x%02x/0x%02x, then latching 0x42:\n",
		targetSub, targetReg, targetReg + 1);

	int from = oneChannel > 0 ? oneChannel : 13;
	int to = oneChannel > 0 ? oneChannel : 62;
	int hits = 0;

	for (int ch = from; ch <= to; ch++) {
		int v = 3312 + 42 * (ch - 13);				// V = 7 x f_MHz
		Write(device, targetSub, targetReg, (uint8)(v >> 8));
		Write(device, targetSub, (uint8)(targetReg + 1), (uint8)(v & 0xFF));
		// Latch the new value the way the init pulse does.
		Write(device, kDemod, 0x42, 0x01);
		Write(device, kDemod, 0x42, 0x00);
		snooze(150000);								// settle + acquire

		uint8 now[64];
		if (!ReadBlock(device, kDemod, 0x00, now, 64))
			continue;

		int changed = 0;
		char detail[256];
		detail[0] = '\0';
		for (int i = 0; i < 64; i++) {
			if (jitter[i] || now[i] == base[i])
				continue;
			// The registers we just wrote are not a reaction.
			if (targetSub == kDemod && i >= targetReg && i <= targetReg + 1)
				continue;
			char one[24];
			snprintf(one, sizeof(one), " %02x:%02x->%02x", i, base[i], now[i]);
			strncat(detail, one, sizeof(detail) - strlen(detail) - 1);
			changed++;
		}

		if (changed > 0 || oneChannel > 0) {
			printf("  UHF %2d  f=%d MHz  V=%4d%s\n", ch, 473 + 6 * (ch - 13), v,
				changed > 0 ? detail : "  (no change)");
			if (changed > 0)
				hits++;
		}
	}

	printf("\n%d channel(s) moved a demod register outside its own jitter.\n",
		hits);
	if (hits == 0) {
		printf("No reaction on any channel. The demod is %s; with it running "
			"and\nno register reacting, this is a real negative for reception "
			"at\nthis antenna - which in Korea is the expected result. Try\n"
			"--target for a different frequency register, or point it at a "
			"signal.\n", alive > 0 ? "running" : "NOT running (see warning)");
	}
	return 0;
}
