// Read the tuner's register space and report what answers.
//
// Everything needed to talk to this module works: the firmware loads, the
// vendor commands are accepted, and the register bridge reaches the chips
// behind it. What is not known is which registers mean what - the tuning
// values are computed at run time rather than stored, so reading the register
// space on live hardware is the way to find the status registers.
//
// This is the other route: ask the hardware. A demodulator has status
// registers that change on their own - signal quality, lock state, AGC - and
// those are findable without knowing the datasheet, by reading the space
// twice and seeing what moved.
//
// Reads only. `bRequest 0x21` is the register-read command the firmware
// decodes; it changes nothing.
//
// Sub-devices on the bus: 0x50 (EEPROM), 0x60 and 0x6C (RF front end), 0x6E
// (demodulator).
//
// Build:  setarch x86 g++ -o reg_scan reg_scan.cpp -ldevice
// Run:    ./reg_scan /dev/bus/usb/3/2              scan 0x60 and 0x6c
//         ./reg_scan --dev 6c --from 0 --to ff     one sub-device, a range
//         ./reg_scan --watch 6c e0                 read one register 20 times

#include <USBKit.h>
#include <OS.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8 kIn = 0xC0;			// device to host, vendor, device
static const uint8 kOut = 0x40;
static const uint8 kRegisterRead = 0x21;
static const uint8 kSetMode = 0x23;
// 0x25 returns the byte a preceding 0x21 produced - see ReadRegister.
static const uint8 kFetchResult = 0x25;

// The firmware spins on an internal busy flag before each command, so give
// it room between requests rather than assuming the wait is free.
static const bigtime_t kSettle = 4000;

// Short on purpose. A register that is going to answer does so in
// microseconds; anything past this is a hole in the address space, and the
// scan's whole job is to find those quickly rather than wait on each one.
static const bigtime_t kReadTimeout = 250000;


// BUSBDevice::ControlTransfer has no timeout of its own, and a register the
// chip does not implement simply never answers - so a plain loop over 256
// addresses stops dead on the first hole. (It did: an earlier version of this
// tool ran fourteen minutes and printed nothing.) Every read therefore runs on
// its own thread with a deadline.
//
// The cost is that each timeout leaks a thread parked inside the USB driver,
// which killing the process does not reclaim - see README.md. That is why the
// default range is small: scan what you need, then power-cycle.
struct ReadJob {
	BUSBDevice*	device;
	uint8		request;
	uint16		index;
	uint16		length;
	uint8*		buffer;
	ssize_t		result;
	sem_id		done;
};


static status_t
ReadThread(void* cookie)
{
	ReadJob* job = (ReadJob*)cookie;
	// A zero-length command still needs a valid pointer: usb_raw rejects NULL
	// even when it will read nothing through it.
	job->result = job->device->ControlTransfer(kIn, job->request, 0,
		job->index, job->length, job->buffer);
	release_sem(job->done);
	return B_OK;
}


// Reading a register takes two commands, which is why a single 0x21 returns
// nothing. The firmware's 0x21 handler performs the bus access and returns
// with `CLR C` - it never touches EP0BUF - so there is no data stage to
// collect. The result is fetched afterwards by 0x25, whose whole body is:
//
//     MOV DPTR,#2539h ; MOVX A,@DPTR    ; the byte the access produced
//     MOV DPTR,#E740h ; MOVX @DPTR,A    ; EP0BUF
//     ... set the byte count to 1
//
// So: 0x21 asks, 0x25 collects. The first version of this tool issued only
// the first half and concluded, wrongly, that nothing answered.
static bool
Transfer(BUSBDevice& device, uint8 request, uint16 index, uint16 length,
	uint8* buffer)
{
	ReadJob job;
	job.device = &device;
	job.request = request;
	job.index = index;
	job.length = length;
	job.buffer = buffer;
	job.result = 0;
	job.done = create_sem(0, "reg-io");

	thread_id thread = spawn_thread(ReadThread, "reg-io", B_NORMAL_PRIORITY,
		&job);
	if (thread < 0) {
		delete_sem(job.done);
		return false;
	}
	resume_thread(thread);

	status_t waited = acquire_sem_etc(job.done, 1, B_RELATIVE_TIMEOUT,
		kReadTimeout);
	delete_sem(job.done);
	snooze(kSettle);

	return waited == B_OK && job.result == (ssize_t)length;
}


static bool
ReadRegister(BUSBDevice& device, uint8 subDevice, uint8 reg, uint8* _value)
{
	uint8 scratch = 0;

	// Ask. wIndexH selects the sub-device, wIndexL the register.
	if (!Transfer(device, kRegisterRead, (uint16)((subDevice << 8) | reg), 0,
			&scratch)) {
		return false;
	}

	// Collect.
	uint8 reply = 0;
	if (!Transfer(device, kFetchResult, 0, 1, &reply))
		return false;

	*_value = reply;
	return true;
}


static void
Scan(BUSBDevice& device, uint8 subDevice, int from, int to)
{
	uint8 first[256];
	uint8 second[256];
	bool ok[256];
	int answered = 0;

	printf("\n=== sub-device 0x%02x, registers 0x%02x-0x%02x ===\n",
		subDevice, from, to);

	for (int reg = from; reg <= to; reg++) {
		ok[reg] = ReadRegister(device, subDevice, (uint8)reg, &first[reg]);
		if (ok[reg])
			answered++;
		// Progress as it goes: a scan that prints nothing for minutes is
		// indistinguishable from one that has hung, and that distinction
		// matters more than tidy output.
		if ((reg & 0x0F) == 0x0F) {
			printf("  ..0x%02x  %d answered so far\n", reg, answered);
			fflush(stdout);
		}
	}

	// A second pass a moment later: anything that differs is live state
	// rather than configuration, which is exactly what a lock or signal
	// register looks like.
	snooze(300000);
	for (int reg = from; reg <= to; reg++) {
		if (ok[reg] && !ReadRegister(device, subDevice, (uint8)reg,
				&second[reg])) {
			ok[reg] = false;
		}
	}

	printf("      x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 xa xb xc xd xe xf\n");
	for (int row = from & 0xF0; row <= to; row += 16) {
		printf("  %02x: ", row);
		for (int col = 0; col < 16; col++) {
			int reg = row + col;
			if (reg < from || reg > to)
				printf("   ");
			else if (!ok[reg])
				printf("-- ");
			else
				printf("%02x ", first[reg]);
		}
		printf("\n");
	}

	printf("  %d of %d registers answered\n", answered, to - from + 1);

	int changed = 0;
	for (int reg = from; reg <= to; reg++) {
		if (!ok[reg] || first[reg] == second[reg])
			continue;
		if (changed == 0)
			printf("\n  registers that changed between passes:\n");
		printf("    0x%02x: %02x -> %02x\n", reg, first[reg], second[reg]);
		changed++;
	}
	if (changed == 0)
		printf("  nothing changed between passes\n");
	else {
		printf("  %d live register(s) - these are status, not "
			"configuration\n", changed);
	}
}


int
main(int argc, char** argv)
{
	const char* path = "/dev/bus/usb/3/2";
	int only = -1;
	int from = 0x00;
	// 0x3F rather than 0xFF by default: each unanswered register costs a
	// timeout and a parked thread, so start narrow and widen deliberately.
	int to = 0x3F;
	int watchDevice = -1;
	int watchRegister = -1;

	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "/dev/", 5) == 0)
			path = argv[i];
		else if (strcmp(argv[i], "--dev") == 0 && i + 1 < argc)
			only = (int)strtol(argv[++i], NULL, 16);
		else if (strcmp(argv[i], "--from") == 0 && i + 1 < argc)
			from = (int)strtol(argv[++i], NULL, 16);
		else if (strcmp(argv[i], "--to") == 0 && i + 1 < argc)
			to = (int)strtol(argv[++i], NULL, 16);
		else if (strcmp(argv[i], "--watch") == 0 && i + 2 < argc) {
			watchDevice = (int)strtol(argv[++i], NULL, 16);
			watchRegister = (int)strtol(argv[++i], NULL, 16);
		} else {
			fprintf(stderr, "unrecognised argument: %s\n", argv[i]);
			return 1;
		}
	}

	setvbuf(stdout, NULL, _IONBF, 0);

	BUSBDevice device(path);
	if (device.InitCheck() != B_OK) {
		fprintf(stderr, "cannot open %s\n", path);
		return 1;
	}

	printf("%s  %04x:%04x  \"%s\"  bcdDevice 0x%04x\n", path,
		device.VendorID(), device.ProductID(), device.ProductString(),
		device.Descriptor()->device_version);

	if (device.Descriptor()->device_version == 0) {
		printf("\nStill the boot loader - run upload_fw first.\n");
		return 1;
	}

	// Mode 1 is what the firmware requires before it will service register
	// access at all; it reads back as 1 already on a fresh start, but say so
	// explicitly rather than depending on that.
	uint8 unused = 0;
	device.ControlTransfer(kOut, kSetMode, 1, 0, 0, &unused);
	snooze(kSettle);

	if (watchDevice >= 0) {
		printf("\nwatching sub-device 0x%02x register 0x%02x:\n  ",
			watchDevice, watchRegister);
		for (int i = 0; i < 20; i++) {
			uint8 value = 0;
			if (ReadRegister(device, (uint8)watchDevice,
					(uint8)watchRegister, &value)) {
				printf("%02x ", value);
			} else {
				printf("-- ");
			}
			snooze(100000);
		}
		printf("\n");
		return 0;
	}

	if (only >= 0) {
		Scan(device, (uint8)only, from, to);
		return 0;
	}

	Scan(device, 0x60, from, to);

	printf("\nRegisters that answer at all tell us the bus reaches that chip;\n"
		"registers that change on their own are its status. A demodulator\n"
		"with no signal still moves its AGC, so a live register here is the\n"
		"handle for everything after this.\n");
	return 0;
}
