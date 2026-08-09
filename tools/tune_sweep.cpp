// Sweep the demodulator's tuning register and watch for the lock status to
// move.
//
// This is the payoff of everything before it. An early reading of the tuning
// path suggested selecting a channel is not a PLL word at all - two bytes,
// written to demodulator registers 0x64 and 0x67 between a latch:
//
//     0x42 = 0x10          open the latch
//     0x64 = <per channel>
//     0x67 = <per channel>
//     0x42 = 0x00          close it
//
// and the code then reads registers 0x08, 0x09 and 0x0A - which is where a
// demodulator reports lock and signal quality. Those two bytes come from a
// table the application fills at run time, so they are not in the binary; but
// two bytes is a small enough space to search when the hardware itself can be
// asked whether a value worked.
//
// The sub-device is 0x6E, which reads back, so unlike the write-only front
// end every step here can be confirmed.
//
// Baseline first: the status registers are read several times with nothing
// written, because at least one demodulator register moves on its own and
// without knowing which, every reading looks like a result.
//
// Build:  setarch x86 g++ -o tune_sweep tune_sweep.cpp -ldevice
// Run:    ./tune_sweep                 sweep 0x64 over 0x00-0xff
//         ./tune_sweep --reg 67        sweep 0x67 instead
//         ./tune_sweep --from 0 --to 3f

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

static const bigtime_t kTimeout = 1500000;
static const bigtime_t kSettle = 8000;

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
	job.done = create_sem(0, "sweep");

	thread_id thread = spawn_thread(Entry, "sweep", B_NORMAL_PRIORITY, &job);
	if (thread < 0) {
		delete_sem(job.done);
		return -1;
	}
	resume_thread(thread);

	status_t waited = acquire_sem_etc(job.done, 1, B_RELATIVE_TIMEOUT,
		kTimeout);
	delete_sem(job.done);
	snooze(kSettle);
	return waited == B_OK ? job.result : -1;
}


static bool
Write(BUSBDevice& device, uint8 reg, uint8 value)
{
	uint8 byte = value;
	return Transfer(device, kOut, kRegisterWrite, 1,
		(uint16)((kDemod << 8) | reg), 1, &byte) == 1;
}


// Registers 0x08-0x0A in one transfer - the three the tuning code reads back.
static bool
ReadStatus(BUSBDevice& device, uint8* out)
{
	return Transfer(device, kIn, kRegisterRead, 3,
		(uint16)((kDemod << 8) | 0x08), 3, out) == 3;
}


int
main(int argc, char** argv)
{
	int reg = 0x64;
	int from = 0x00;
	int to = 0xFF;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--reg") == 0 && i + 1 < argc)
			reg = (int)strtol(argv[++i], NULL, 16);
		else if (strcmp(argv[i], "--from") == 0 && i + 1 < argc)
			from = (int)strtol(argv[++i], NULL, 16);
		else if (strcmp(argv[i], "--to") == 0 && i + 1 < argc)
			to = (int)strtol(argv[++i], NULL, 16);
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

	printf("%s ver %04x\n\n", device.ProductString(),
		device.Descriptor()->device_version);

	// The reset pulse the tuning path starts with.
	Write(device, 0x38, 0x80);
	Write(device, 0x38, 0x00);
	printf("reset pulse sent (0x38 = 0x80, 0x00)\n");

	// How much do the status registers move on their own?
	uint8 base[3] = { 0, 0, 0 };
	uint8 seen[8][3];
	int seenCount = 0;
	for (int i = 0; i < 8; i++) {
		if (!ReadStatus(device, seen[i])) {
			printf("status read failed - stopping\n");
			return 1;
		}
		snooze(60000);
		seenCount++;
	}
	memcpy(base, seen[seenCount - 1], 3);

	bool jitter[3] = { false, false, false };
	for (int i = 1; i < seenCount; i++) {
		for (int k = 0; k < 3; k++) {
			if (seen[i][k] != seen[0][k])
				jitter[k] = true;
		}
	}
	printf("baseline 0x08,0x09,0x0a = %02x %02x %02x   self-moving:",
		base[0], base[1], base[2]);
	for (int k = 0; k < 3; k++)
		printf(" %s", jitter[k] ? "yes" : "no");
	printf("\n\nsweeping register 0x%02x, 0x%02x-0x%02x:\n", reg, from, to);

	int hits = 0;
	for (int v = from; v <= to; v++) {
		Write(device, 0x42, 0x10);
		Write(device, (uint8)reg, (uint8)v);
		Write(device, 0x42, 0x00);
		snooze(30000);

		uint8 now[3];
		if (!ReadStatus(device, now))
			continue;

		bool interesting = false;
		for (int k = 0; k < 3; k++) {
			if (!jitter[k] && now[k] != base[k])
				interesting = true;
		}
		if (interesting) {
			printf("  0x%02x -> status %02x %02x %02x\n", v, now[0], now[1],
				now[2]);
			hits++;
		}
		if ((v & 0x1F) == 0x1F) {
			printf("  ..0x%02x (%d responses so far)\n", v, hits);
			fflush(stdout);
		}
	}

	printf("\n%d value(s) changed a status register that does not move on "
		"its own.\n", hits);
	if (hits == 0) {
		printf("Nothing responded. Either these are not the tuning registers\n"
			"after all, or the front end has to be brought up first for the\n"
			"demodulator to have anything to lock onto.\n");
	}
	return 0;
}
