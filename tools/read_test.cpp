// Find out how a register read actually returns its result - in seven
// transfers, not seven hundred.
//
// WHY THIS IS SO SMALL
//
// Surveying the register space does not work on this hardware. Each transfer
// that goes unanswered leaves a thread parked inside Haiku's USB driver, and
// killing the process does not reclaim it; a few dozen of those and the
// device stops answering anything at all. A 64-register scan wedged it twice.
// So the budget here is a handful of transfers per boot, spent deliberately.
//
// The question is narrow. `bRequest 0x21` performs a register access - the
// firmware decodes it, the Windows application issues it - but its handler
// returns without ever writing EP0BUF:
//
//     0fd5  ...  LCALL 0x15fb   ; do the bus access
//     0ff7       LJMP  0x1081   ; CLR C, return - no data stage
//
// while `0x25` does nothing but hand back one byte:
//
//     103d  MOV DPTR,#2539h ; MOVX A,@DPTR   ; whatever the access produced
//     1041  MOV DPTR,#E740h ; MOVX @DPTR,A   ; EP0BUF
//
// That reads like ask-then-collect, but it has not been proven on hardware.
// This tries the plausible spellings and reports which one returns a byte.
//
// Every transfer is timed, so a wrong guess costs one parked thread rather
// than the session.
//
// Build:  setarch x86 g++ -o read_test read_test.cpp -ldevice
// Run:    ./read_test /dev/bus/usb/3/2

#include <USBKit.h>
#include <OS.h>

#include <stdio.h>
#include <string.h>

static const uint8 kIn = 0xC0;
static const uint8 kOut = 0x40;
static const bigtime_t kTimeout = 1500000;
static const bigtime_t kSettle = 10000;

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


// Returns bytes transferred, or -1 on timeout.
static ssize_t
Try(BUSBDevice& device, const char* what, uint8 requestType, uint8 request,
	uint16 value, uint16 index, uint16 length, uint8* buffer)
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
	job.done = create_sem(0, "read-test");

	printf("  %-34s ", what);
	fflush(stdout);

	thread_id thread = spawn_thread(Entry, "read-test", B_NORMAL_PRIORITY,
		&job);
	if (thread < 0) {
		delete_sem(job.done);
		printf("could not start a thread\n");
		return -1;
	}
	resume_thread(thread);

	status_t waited = acquire_sem_etc(job.done, 1, B_RELATIVE_TIMEOUT,
		kTimeout);
	delete_sem(job.done);

	if (waited != B_OK) {
		printf("TIMED OUT (one thread now parked)\n");
		return -1;
	}
	if (job.result < 0) {
		printf("error: %s\n", strerror(job.result));
	} else {
		printf("%" B_PRIdSSIZE " byte(s)", job.result);
		if (job.result > 0 && buffer != NULL)
			printf(" = %02x", buffer[0]);
		printf("\n");
	}
	snooze(kSettle);
	return job.result;
}


int
main(int argc, char** argv)
{
	const char* path = argc > 1 ? argv[1] : "/dev/bus/usb/3/2";

	setvbuf(stdout, NULL, _IONBF, 0);

	BUSBDevice device(path);
	if (device.InitCheck() != B_OK) {
		fprintf(stderr, "cannot open %s\n", path);
		return 1;
	}

	printf("%s  %04x:%04x  \"%s\"  bcdDevice 0x%04x\n\n", path,
		device.VendorID(), device.ProductID(), device.ProductString(),
		device.Descriptor()->device_version);

	if (device.Descriptor()->device_version == 0) {
		printf("Still the boot loader - run upload_fw first.\n");
		return 1;
	}

	uint8 reply = 0;

	// 1. The known-good transfer. If this fails, the device is wedged from a
	//    previous run and nothing below means anything - stop and power cycle.
	printf("baseline:\n");
	reply = 0;
	if (Try(device, "0x24 get mode, IN, 1 byte", kIn, 0x24, 0, 0, 1, &reply)
			!= 1) {
		printf("\nThe device is not answering a command that is known to "
			"work.\nPower-cycle it and upload the firmware again before "
			"reading anything into the results above.\n");
		return 1;
	}
	printf("  (mode is %d - register access needs mode 1)\n\n", reply);

	// 2. Ask-then-collect, in the two spellings the firmware could mean.
	printf("ask with 0x21, then collect with 0x25:\n");
	Try(device, "0x21 zero-length OUT", kOut, 0x21, 0, 0x6000, 0, NULL);
	reply = 0;
	Try(device, "0x25 collect, IN, 1 byte", kIn, 0x25, 0, 0, 1, &reply);

	printf("\nsame, but asking as an IN transfer:\n");
	Try(device, "0x21 zero-length IN", kIn, 0x21, 0, 0x6000, 0, NULL);
	reply = 0;
	Try(device, "0x25 collect, IN, 1 byte", kIn, 0x25, 0, 0, 1, &reply);

	// 3. The one-step spelling, for completeness: if the firmware does fill
	//    EP0BUF somewhere this would show it.
	printf("\none-step, for comparison:\n");
	reply = 0;
	Try(device, "0x21 IN, 1 byte", kIn, 0x21, 0, 0x6000, 1, &reply);

	printf("\nWhichever line returned a byte is the spelling to use. If the\n"
		"0x25 lines return a byte but always the same one, it is holding a\n"
		"stale result rather than this read's - which would mean the access\n"
		"needs mode 1 set first, or a different sub-device.\n");
	return 0;
}
