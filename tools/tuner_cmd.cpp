// Drive the CXD9192's vendor commands, and see whether the data path works.
//
// The command set came out of the firmware, not documentation. What the
// firmware gives is the transport: set a mode, reset the FIFOs, read and write
// registers on the internal bus. What it does not give is which registers to
// write to select a channel, because that is computed in the application
// rather than stored in the driver or the firmware.
//
// So this answers a narrower question first, and a useful one: with the mode
// set and the FIFOs reset, does *anything* come out of the data endpoint? If
// even null transport packets appear, the whole path from demodulator to USB
// is working and only the tuning values are missing. If nothing appears, the
// demodulator has to be configured before it will produce so much as a null,
// and the application is the only way forward.
//
// Commands, all vendor requests to the device:
//
//   0x23  set mode      wValueL = mode      (mode 1 enables register access)
//   0x24  get mode      returns one byte in EP0BUF
//   0x27  FIFO reset    no parameters
//   0x21  register read  wIndexH = target, wIndexL = address -> one byte
//   0x20  register write wIndexH = target, wIndexL = address, wValueL = value
//
// Before dispatching any of these the firmware spins on an internal busy flag,
// so they are not safe to issue back to back at full speed; this leaves a
// short gap between them.
//
// Everything here is a control transfer to a receive-only demodulator, and
// the firmware itself is volatile - a power cycle returns the module to its
// boot loader with nothing retained. There is no persistent state to damage.
//
// Build:  setarch x86 g++ -o tuner_cmd tuner_cmd.cpp -ldevice
// Run:    ./tuner_cmd /dev/bus/usb/3/2            - probe sequence, then read
//         ./tuner_cmd --mode 1                    - just set the mode
//         ./tuner_cmd --get-mode
//         ./tuner_cmd --reg-read 7f 20            - target 0x7f, address 0x20
//         ./tuner_cmd --no-read                   - skip the endpoint read

#include <USBKit.h>
#include <OS.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8 kOut = 0x40;		// host to device, vendor, device
static const uint8 kIn = 0xC0;		// device to host, vendor, device

static const uint8 kSetMode = 0x23;
static const uint8 kGetMode = 0x24;
static const uint8 kFifoReset = 0x27;
static const uint8 kRegisterWrite = 0x20;
static const uint8 kRegisterRead = 0x21;

// The firmware waits on its own busy flag before each command; give it room
// rather than assuming the wait is free.
static const bigtime_t kSettle = 20000;

static const bigtime_t kReadTimeout = 4000000;
static const size_t kReadSize = 8320;	// 20 x 416, a whole number of packets


struct ReadJob {
	const BUSBEndpoint*	endpoint;
	uint8*				buffer;
	size_t				size;
	ssize_t				result;
	sem_id				done;
};


static status_t
ReadThread(void* cookie)
{
	ReadJob* job = (ReadJob*)cookie;
	job->result = job->endpoint->BulkTransfer(job->buffer, job->size);
	release_sem(job->done);
	return B_OK;
}


static int
DetectTransportStream(const uint8* data, size_t size)
{
	const int kSizes[] = { 188, 192, 204 };
	for (size_t s = 0; s < sizeof(kSizes) / sizeof(kSizes[0]); s++) {
		int packet = kSizes[s];
		for (size_t start = 0; start < (size_t)packet && start < size; start++) {
			int hits = 0;
			for (size_t off = start; off < size; off += packet) {
				if (data[off] != 0x47)
					break;
				hits++;
			}
			if (hits >= 5)
				return packet;
		}
	}
	return 0;
}


static bool
Command(BUSBDevice& device, const char* what, uint8 request, uint16 value,
	uint16 index, uint16 length, void* buffer)
{
	// usb_raw rejects a transfer whose data pointer is NULL even when the
	// length is zero, which is how a no-data-stage vendor command has to be
	// expressed. Handing it a valid pointer it will never read keeps the
	// length honest - the first attempt passed NULL and every zero-length
	// command came back "General system error", which looked like the device
	// refusing when it was the host API objecting.
	uint8 unused = 0;
	void* data = buffer != NULL ? buffer : &unused;
	bool in = request == kGetMode || request == kRegisterRead;

	ssize_t result = device.ControlTransfer(in ? kIn : kOut, request, value,
		index, length, data);
	printf("  %-28s request 0x%02x value 0x%04x index 0x%04x -> ", what,
		request, value, index);
	if (result < 0) {
		printf("%s\n", strerror(result));
		return false;
	}
	printf("%" B_PRIdSSIZE " byte(s)", result);
	if (result > 0 && buffer != NULL) {
		printf(" =");
		for (ssize_t i = 0; i < result && i < 8; i++)
			printf(" %02x", ((uint8*)buffer)[i]);
	}
	printf("\n");
	snooze(kSettle);
	return true;
}


int
main(int argc, char** argv)
{
	const char* path = "/dev/bus/usb/3/2";
	int mode = -1;
	bool getMode = false;
	bool doRead = true;
	bool sequence = true;
	long regTarget = -1;
	long regAddress = -1;

	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "/dev/", 5) == 0)
			path = argv[i];
		else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
			mode = (int)strtol(argv[++i], NULL, 0);
			sequence = false;
		} else if (strcmp(argv[i], "--get-mode") == 0) {
			getMode = true;
			sequence = false;
		} else if (strcmp(argv[i], "--reg-read") == 0 && i + 2 < argc) {
			regTarget = strtol(argv[++i], NULL, 16);
			regAddress = strtol(argv[++i], NULL, 16);
			sequence = false;
		} else if (strcmp(argv[i], "--no-read") == 0) {
			doRead = false;
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

	printf("%s  %04x:%04x  \"%s\"  bcdDevice 0x%04x\n\n", path,
		device.VendorID(), device.ProductID(), device.ProductString(),
		device.Descriptor()->device_version);

	if (device.Descriptor()->device_version == 0) {
		printf("Still the boot loader. Run upload_fw first.\n");
		return 1;
	}

	uint8 reply[8];

	if (getMode) {
		memset(reply, 0, sizeof(reply));
		Command(device, "get mode", kGetMode, 0, 0, 1, reply);
		return 0;
	}

	if (mode >= 0) {
		Command(device, "set mode", kSetMode, (uint16)mode, 0, 0, NULL);
		return 0;
	}

	if (regTarget >= 0) {
		memset(reply, 0, sizeof(reply));
		Command(device, "register read", kRegisterRead, 0,
			(uint16)((regTarget << 8) | regAddress), 1, reply);
		return 0;
	}

	if (sequence) {
		printf("bringing the module up:\n");
		memset(reply, 0, sizeof(reply));
		Command(device, "get mode (before)", kGetMode, 0, 0, 1, reply);
		Command(device, "set mode 1", kSetMode, 1, 0, 0, NULL);
		memset(reply, 0, sizeof(reply));
		Command(device, "get mode (after)", kGetMode, 0, 0, 1, reply);
		Command(device, "FIFO reset", kFifoReset, 0, 0, 0, NULL);
		// Register 0 of target 0x7f, purely to see whether the bus answers -
		// a read cannot change anything, and a plausible value would confirm
		// the register path as well as the command path.
		memset(reply, 0, sizeof(reply));
		Command(device, "register read 7f:00", kRegisterRead, 0, 0x7f00, 1,
			reply);
	}

	if (!doRead)
		return 0;

	const BUSBConfiguration* config = device.ActiveConfiguration();
	if (config == NULL)
		config = device.ConfigurationAt(0);
	const BUSBInterface* interface = config != NULL
		? config->InterfaceAt(0) : NULL;
	if (interface == NULL) {
		fprintf(stderr, "no interface 0\n");
		return 1;
	}

	const BUSBEndpoint* data = NULL;
	for (uint32 e = 0; e < interface->CountEndpoints(); e++) {
		const BUSBEndpoint* endpoint = interface->EndpointAt(e);
		if (endpoint != NULL && endpoint->IsBulk() && endpoint->IsInput()
			&& endpoint->MaxPacketSize() > 64) {
			data = endpoint;
		}
	}
	if (data == NULL) {
		fprintf(stderr, "no large bulk IN endpoint\n");
		return 1;
	}

	printf("\nreading endpoint 0x%02x (%u-byte packets): ",
		data->Descriptor()->endpoint_address, data->MaxPacketSize());

	uint8* buffer = (uint8*)malloc(kReadSize);
	if (buffer == NULL)
		return 1;
	memset(buffer, 0, kReadSize);

	ReadJob job;
	job.endpoint = data;
	job.buffer = buffer;
	job.size = kReadSize;
	job.result = 0;
	job.done = create_sem(0, "tuner-read");

	thread_id thread = spawn_thread(ReadThread, "tuner-read",
		B_NORMAL_PRIORITY, &job);
	if (thread < 0) {
		delete_sem(job.done);
		return 1;
	}
	resume_thread(thread);

	if (acquire_sem_etc(job.done, 1, B_RELATIVE_TIMEOUT, kReadTimeout)
			!= B_OK) {
		printf("nothing within %" B_PRIdBIGTIME "s\n",
			kReadTimeout / 1000000);
		printf("\nThe data path is not producing anything yet. The mode and\n"
			"FIFO commands were accepted, so the missing piece is the\n"
			"demodulator's own configuration - which means the tuning\n"
			"sequence from Sony's application.\n");
		delete_sem(job.done);
		return 0;
	}
	delete_sem(job.done);

	if (job.result < 0) {
		printf("error: %s\n", strerror(job.result));
		return 1;
	}

	printf("%" B_PRIdSSIZE " bytes\n", job.result);
	if (job.result > 0) {
		int packet = DetectTransportStream(buffer, job.result);
		if (packet > 0) {
			printf("  >>> MPEG-2 TRANSPORT STREAM, %d-byte packets\n", packet);
			FILE* out = fopen("tuner-capture.ts", "wb");
			if (out != NULL) {
				fwrite(buffer, 1, job.result, out);
				fclose(out);
				printf("  >>> saved to tuner-capture.ts\n");
			}
			printf("\nThe path works end to end. What is left is tuning.\n");
		} else {
			printf("  first bytes: ");
			for (ssize_t i = 0; i < job.result && i < 64; i++)
				printf("%02x ", buffer[i]);
			printf("\n\nData, but not a transport stream - so the endpoint is\n"
				"alive and this is either framed differently or the\n"
				"demodulator is emitting something other than TS yet.\n");
		}
	}

	free(buffer);
	return 0;
}
