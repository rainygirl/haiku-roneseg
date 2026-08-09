// Write a candidate PLL word to the RF front end and watch the demodulator.
//
// By this point the transport is solved and instrumented:
//
//   sub-device 0x50   the I2C EEPROM - reads back the VID/PID the device
//                     enumerates with, which is what proves the bus works
//   sub-device 0x6E   the CXD9192 demodulator - 128 readable registers,
//                     writes verified by read-back, and register 0x14 changes
//                     on its own
//   sub-device 0x60   the RF front end - every register reads 00, and it is
//                     where the tuning path sends ten bytes per tune, to
//                     registers 0xA0-0xA9
//
// What is still unknown is the encoding of those ten bytes. This tries a
// candidate and looks for the demodulator to react.
//
// The candidate comes from arithmetic rather than guesswork. Japan's ISDB-T
// raster is 473 + 1/7 MHz with 6 MHz spacing, so N = 7 x f_MHz is an exact
// integer for every channel - 3312 for UHF 13, +42 per channel after that.
// A PLL word built on the frequency almost certainly encodes that number.
//
// Reading the demodulator twice before writing anything establishes which
// registers move on their own; without that baseline every reading looks like
// a result. Only changes outside that set are reported.
//
// Nothing here is persistent: the firmware lives in the module's RAM and a
// power cycle returns it to its boot loader.
//
// Build:  setarch x86 g++ -o tune_try tune_try.cpp -ldevice
// Run:    ./tune_try                 UHF 13
//         ./tune_try --channel 27
//         ./tune_try --raw 0c f0 00 00 00

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
static const uint8 kTunerBase = 0xA0;

static const bigtime_t kTimeout = 2000000;
static const bigtime_t kSettle = 15000;

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
	job.done = create_sem(0, "tune");

	thread_id thread = spawn_thread(Entry, "tune", B_NORMAL_PRIORITY, &job);
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


// wValue is the count of registers, which is what lets a whole page come back
// in one transfer.
static bool
ReadBlock(BUSBDevice& device, uint8 subDevice, uint8 base, uint8* out, int n)
{
	return Transfer(device, kIn, kRegisterRead, (uint16)n,
		(uint16)((subDevice << 8) | base), (uint16)n, out) == n;
}


static bool
WriteRegister(BUSBDevice& device, uint8 subDevice, uint8 reg, uint8 value)
{
	uint8 byte = value;
	return Transfer(device, kOut, kRegisterWrite, 1,
		(uint16)((subDevice << 8) | reg), 1, &byte) == 1;
}


static void
ReadDemod(BUSBDevice& device, uint8* out)
{
	memset(out, 0, 128);
	ReadBlock(device, kDemod, 0x00, out, 64);
	ReadBlock(device, kDemod, 0x40, out + 64, 64);
}


static uint8
DemodRegister(int i)
{
	return (uint8)(i < 64 ? i : 0x40 + (i - 64));
}


int
main(int argc, char** argv)
{
	int channel = 13;
	uint8 word[5] = { 0, 0, 0, 0, 0 };
	bool raw = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc)
			channel = (int)strtol(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "--raw") == 0 && i + 5 < argc) {
			for (int k = 0; k < 5; k++)
				word[k] = (uint8)strtol(argv[++i], NULL, 16);
			raw = true;
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

	int n = 3312 + 42 * (channel - 13);
	if (!raw) {
		word[0] = (uint8)(n >> 8);
		word[1] = (uint8)(n & 0xFF);
	}

	printf("%s ver %04x\n", device.ProductString(),
		device.Descriptor()->device_version);
	printf("UHF %d -> N = %d (0x%04x), writing %02x %02x %02x %02x %02x "
		"to 0x%02x:0x%02x..\n\n", channel, n, n, word[0], word[1], word[2],
		word[3], word[4], kTuner, kTunerBase);

	// Baseline: two reads with nothing in between, so registers that move on
	// their own can be excluded from the result.
	uint8 a[128], b[128], c[128];
	ReadDemod(device, a);
	snooze(400000);
	ReadDemod(device, b);

	bool noisy[128];
	int noisyCount = 0;
	for (int i = 0; i < 128; i++) {
		noisy[i] = a[i] != b[i];
		if (noisy[i])
			noisyCount++;
	}
	printf("baseline: %d register(s) move on their own", noisyCount);
	if (noisyCount > 0) {
		printf(" -");
		for (int i = 0; i < 128; i++) {
			if (noisy[i])
				printf(" %02x", DemodRegister(i));
		}
	}
	printf("\n");

	for (int k = 0; k < 5; k++) {
		if (!WriteRegister(device, kTuner, (uint8)(kTunerBase + k), word[k])) {
			printf("write to 0x%02x failed\n", kTunerBase + k);
			return 1;
		}
	}
	printf("wrote the PLL word\n");

	// A PLL needs time to settle and the demodulator time to notice.
	snooze(800000);
	ReadDemod(device, c);

	printf("\nchanges after the write, excluding the noisy registers:\n");
	int changed = 0;
	for (int i = 0; i < 128; i++) {
		if (noisy[i] || b[i] == c[i])
			continue;
		printf("   reg %02x: %02x -> %02x\n", DemodRegister(i), b[i], c[i]);
		changed++;
	}
	if (changed == 0) {
		printf("   none\n\nThe demodulator did not react. Either this is not "
			"the encoding,\nor the front end needs its own initialisation "
			"before a PLL word\nmeans anything to it.\n");
	} else {
		printf("\n%d register(s) responded to the write - that is the signal "
			"to follow.\n", changed);
	}
	return 0;
}
