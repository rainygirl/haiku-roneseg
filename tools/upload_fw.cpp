// Upload the One-Seg tuner's firmware and bring its 8051 out of reset.
//
// The module (054c:0279) powers up as a Cypress EZ-USB with no firmware: it
// answers control transfers, reports bcdDevice 0x0000 with no string
// descriptors, and streams on no endpoint. This uploads an 8051 firmware image
// so it renumerates as a working tuner.
//
// The image is a oneseg_fw.rec file: a table of records, each a 4-byte header
// (u16 address, u16 length) followed by that many data bytes. You supply the
// file, extracted from a driver you already have for the device. It loads to
// the EZ-USB FX2 memory map (0xE600 CPUCS, 0xE740 EP0BUF, the 0xE6xx block).
//
// Firmware goes into the 8051's internal RAM, not flash or EEPROM. Unplugging
// the machine, or a reboot, restores the blank bootloader exactly as it was.
// There is nothing here to brick: the worst outcome is an 8051 that runs bad
// code and produces no device, which a power cycle undoes.
//
// The sequence is the documented EZ-USB one, request 0xA0:
//   1. 0xA0 to 0xE600 with 0x01  - hold the 8051 in reset
//   2. 0xA0 to each address       - write the firmware
//   3. 0xA0 to 0xE600 with 0x00  - release it; the device renumerates
//
// Build:  setarch x86 g++ -o upload_fw upload_fw.cpp -ldevice
// Run:    ./upload_fw /dev/bus/usb/3/2 oneseg_fw.rec
//         listusb            # the device should come back, with strings
//
// Add --dry-run to print what would be sent without touching the device.

#include <USBKit.h>
#include <OS.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8 kFirmwareLoad = 0xA0;	// EZ-USB boot loader request
static const uint16 kCpuCsRegister = 0xE600;
static const uint8 kCpuReset = 0x01;
static const uint8 kCpuRun = 0x00;

// Host-to-device, vendor request, recipient device.
static const uint8 kVendorOut = 0x40;


// A wedged device answers nothing and BUSBDevice::ControlTransfer has no
// timeout of its own, so the first transfer would simply never return - which
// is exactly what happened the first time this tool was run, on a boot where
// probe_all had already left threads parked in the driver. One timed transfer
// up front turns that from an indefinite hang into a diagnosis.
//
// Only the reachability check is timed. Once the device has answered once,
// the remaining transfers are left untimed: a timeout leaks a parked thread,
// and 652 of those would be worse than the hang they guard against.
struct ReachJob {
	BUSBDevice*	device;
	uint8*		buffer;
	ssize_t		result;
	sem_id		done;
};


static status_t
ReachThread(void* cookie)
{
	ReachJob* job = (ReachJob*)cookie;
	// GET_DESCRIPTOR(device) - the cheapest request every device must answer,
	// and one that changes nothing.
	job->result = job->device->ControlTransfer(0x80, 0x06, 0x0100, 0, 18,
		job->buffer);
	release_sem(job->done);
	return B_OK;
}


static bool
DeviceAnswers(BUSBDevice& device)
{
	uint8 buffer[18];
	ReachJob job;
	job.device = &device;
	job.buffer = buffer;
	job.result = 0;
	job.done = create_sem(0, "reach");

	thread_id thread = spawn_thread(ReachThread, "reach", B_NORMAL_PRIORITY,
		&job);
	if (thread < 0) {
		delete_sem(job.done);
		return false;
	}
	resume_thread(thread);

	status_t waited = acquire_sem_etc(job.done, 1, B_RELATIVE_TIMEOUT,
		2000000);
	delete_sem(job.done);
	return waited == B_OK && job.result == 18;
}


static status_t
WriteRam(BUSBDevice& device, uint16 address, const void* data, uint16 length,
	bool dryRun)
{
	if (dryRun) {
		printf("  would write %2u byte(s) to 0x%04x\n", length, address);
		return B_OK;
	}

	ssize_t written = device.ControlTransfer(kVendorOut, kFirmwareLoad,
		address, 0, length, const_cast<void*>(data));
	if (written < 0)
		return (status_t)written;
	if (written != (ssize_t)length)
		return B_IO_ERROR;
	return B_OK;
}


static status_t
SetCpu(BUSBDevice& device, bool halted, bool dryRun)
{
	uint8 value = halted ? kCpuReset : kCpuRun;
	printf("%s the 8051 (CPUCS = 0x%02x)\n",
		halted ? "halting" : "releasing", value);
	return WriteRam(device, kCpuCsRegister, &value, 1, dryRun);
}


int
main(int argc, char** argv)
{
	const char* path = "/dev/bus/usb/3/2";
	const char* recordPath = "oneseg_fw.rec";
	bool dryRun = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--dry-run") == 0)
			dryRun = true;
		else if (strncmp(argv[i], "/dev/", 5) == 0)
			path = argv[i];
		else
			recordPath = argv[i];
	}

	FILE* file = fopen(recordPath, "rb");
	if (file == NULL) {
		fprintf(stderr, "cannot open %s\n", recordPath);
		return 1;
	}

	BUSBDevice device(path);
	if (device.InitCheck() != B_OK) {
		fprintf(stderr, "cannot open %s\n", path);
		fclose(file);
		return 1;
	}

	printf("%s  %04x:%04x  bcdDevice 0x%04x%s\n\n", path, device.VendorID(),
		device.ProductID(), device.Descriptor()->device_version,
		dryRun ? "   (dry run)" : "");

	// Worth stating rather than assuming: a device that already has firmware
	// reports a version and strings, and re-loading over the top of running
	// firmware is not what this tool is for.
	if (device.Descriptor()->device_version != 0) {
		printf("This device reports bcdDevice 0x%04x, so it is not in the "
			"blank boot-loader state this tool expects.\nContinuing anyway "
			"would write over running firmware; stopping instead.\n",
			device.Descriptor()->device_version);
		fclose(file);
		return 1;
	}

	if (!dryRun && !DeviceAnswers(device)) {
		fprintf(stderr,
			"The device is not answering control transfers.\n"
			"\n"
			"On this machine that almost always means a previous probe left\n"
			"threads parked in the USB driver - a timed-out transfer does\n"
			"that, and killing the process does not clear them. Check with\n"
			"  ps | grep probe\n"
			"and if anything is listed, reboot before running this. Nothing\n"
			"has been written to the device.\n");
		fclose(file);
		return 1;
	}

	if (SetCpu(device, true, dryRun) != B_OK) {
		fprintf(stderr, "could not halt the 8051 - the device did not accept "
			"request 0xA0, so it is not an EZ-USB boot loader after all\n");
		fclose(file);
		return 1;
	}

	printf("writing firmware from %s\n", recordPath);

	uint32 records = 0;
	uint32 bytes = 0;
	uint8 buffer[64];

	while (true) {
		uint8 header[4];
		if (fread(header, 1, 4, file) != 4)
			break;
		uint16 address = header[0] | (header[1] << 8);
		uint16 length = header[2] | (header[3] << 8);
		if (length == 0)
			break;
		if (length > sizeof(buffer)) {
			fprintf(stderr, "record %u claims %u bytes; file is corrupt\n",
				records, length);
			fclose(file);
			return 1;
		}
		if (fread(buffer, 1, length, file) != length) {
			fprintf(stderr, "record %u is truncated\n", records);
			fclose(file);
			return 1;
		}

		status_t status = WriteRam(device, address, buffer, length, dryRun);
		if (status != B_OK) {
			fprintf(stderr, "\nrecord %u (%u bytes to 0x%04x) failed: %s\n",
				records, length, address, strerror(status));
			// Leaving the 8051 halted would be a worse state to walk away
			// from than either extreme, so release it before giving up.
			SetCpu(device, false, dryRun);
			fclose(file);
			return 1;
		}

		records++;
		bytes += length;
		if (!dryRun && records % 64 == 0) {
			printf("  %u records, %u bytes\n", records, bytes);
			fflush(stdout);
		}
	}
	fclose(file);

	printf("  %u records, %u bytes total\n", records, bytes);

	if (SetCpu(device, false, dryRun) != B_OK) {
		fprintf(stderr, "could not release the 8051\n");
		return 1;
	}

	if (dryRun)
		return 0;

	printf("\nDone. The device should now renumerate - it disappears and\n"
		"comes back, this time with a bcdDevice and strings of its own.\n"
		"Give it a couple of seconds, then run listusb.\n"
		"\n"
		"A power cycle undoes all of this: the firmware is in RAM.\n");
	return 0;
}
