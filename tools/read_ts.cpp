// Read the tuner's data endpoint and say whether a transport stream is
// coming out of it.
//
// Run this after tools/upload_fw.cpp has loaded the firmware. Before the
// upload the device is a Cypress boot loader with Cypress's generic
// descriptors; afterwards it renumerates as "CXD9192 Controller" with the
// endpoints it actually uses:
//
//   interface 0, alternate 0, vendor class 0xff
//     bulk IN, 16-byte packets    - small, so almost certainly status
//     bulk IN, 416-byte packets   - the data stream
//
// There are no OUT endpoints at all, so whatever tells this tuner to start
// and what to tune goes over the control pipe as a vendor request. That
// sequence is not known yet, which is the point of reading first: a
// demodulator that streams as soon as its firmware runs behaves differently
// from one that waits to be told, and the two need different work next.
//
// Reads are timed, because a bulk transfer with nothing to deliver never
// returns on its own. A timeout leaves a thread parked inside the USB driver
// that killing the process does not clear, so this reads each endpoint once
// and then stops - see README.md, "A measurement error worth recording".
//
// Build:  setarch x86 g++ -o read_ts read_ts.cpp -ldevice
// Run:    ./read_ts /dev/bus/usb/3/2

#include <USBKit.h>
#include <OS.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const bigtime_t kTimeout = 4000000;
static const size_t kReadSize = 16384;

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


// Returns the packet size if this looks like MPEG-2 TS, else 0. Five sync
// bytes at the right stride is not something arbitrary binary produces.
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

	printf("%s  %04x:%04x  \"%s\"  bcdDevice 0x%04x\n", path,
		device.VendorID(), device.ProductID(), device.ProductString(),
		device.Descriptor()->device_version);

	if (device.Descriptor()->device_version == 0) {
		printf("\nThis is still the boot loader (bcdDevice 0). Run upload_fw "
			"first.\n");
		return 1;
	}

	const BUSBConfiguration* config = device.ActiveConfiguration();
	if (config == NULL)
		config = device.ConfigurationAt(0);
	if (config == NULL) {
		fprintf(stderr, "no configuration\n");
		return 1;
	}
	const BUSBInterface* interface = config->InterfaceAt(0);
	if (interface == NULL) {
		fprintf(stderr, "no interface 0\n");
		return 1;
	}

	uint8* buffer = (uint8*)malloc(kReadSize);
	if (buffer == NULL)
		return 1;

	bool sawStream = false;

	// Largest packet size first: that is the data endpoint, and the one whose
	// answer decides what happens next.
	for (int pass = 0; pass < 2; pass++) {
		for (uint32 e = 0; e < interface->CountEndpoints(); e++) {
			const BUSBEndpoint* endpoint = interface->EndpointAt(e);
			if (endpoint == NULL || !endpoint->IsBulk()
				|| !endpoint->IsInput()) {
				continue;
			}
			bool big = endpoint->MaxPacketSize() > 64;
			if ((pass == 0) != big)
				continue;

			printf("\nendpoint 0x%02x, %u-byte packets: ",
				endpoint->Descriptor()->endpoint_address,
				endpoint->MaxPacketSize());

			// A whole number of max-size packets, so a short read means the
			// device stopped rather than the buffer ending mid-packet.
			size_t size = (kReadSize / endpoint->MaxPacketSize())
				* endpoint->MaxPacketSize();
			if (size == 0)
				size = endpoint->MaxPacketSize();

			ReadJob job;
			job.endpoint = endpoint;
			job.buffer = buffer;
			job.size = size;
			job.result = 0;
			job.done = create_sem(0, "read-ts");

			memset(buffer, 0, size);
			thread_id thread = spawn_thread(ReadThread, "read-ts",
				B_NORMAL_PRIORITY, &job);
			if (thread < 0) {
				delete_sem(job.done);
				continue;
			}
			resume_thread(thread);

			if (acquire_sem_etc(job.done, 1, B_RELATIVE_TIMEOUT, kTimeout)
					!= B_OK) {
				printf("nothing within %" B_PRIdBIGTIME "s\n",
					kTimeout / 1000000);
				delete_sem(job.done);
				continue;
			}
			delete_sem(job.done);

			if (job.result < 0) {
				printf("error: %s\n", strerror(job.result));
				continue;
			}
			printf("%" B_PRIdSSIZE " bytes\n", job.result);
			if (job.result == 0)
				continue;

			int packet = DetectTransportStream(buffer, job.result);
			if (packet > 0) {
				printf("  >>> MPEG-2 TRANSPORT STREAM, %d-byte packets\n",
					packet);
				sawStream = true;
				FILE* out = fopen("tuner-capture.ts", "wb");
				if (out != NULL) {
					fwrite(buffer, 1, job.result, out);
					fclose(out);
					printf("  >>> saved %" B_PRIdSSIZE
						" bytes to tuner-capture.ts\n", job.result);
				}
			} else {
				printf("  first bytes: ");
				for (ssize_t i = 0; i < job.result && i < 48; i++)
					printf("%02x ", buffer[i]);
				printf("\n");
			}
		}
	}

	free(buffer);

	printf("\n%s\n", sawStream
		? "The tuner is streaming. Point R One-Seg at this endpoint."
		: "No transport stream yet. The firmware is running - the device\n"
		  "renumerated and named itself - so what is missing is the command\n"
		  "that starts it, which has to come from the control pipe.");
	return 0;
}
