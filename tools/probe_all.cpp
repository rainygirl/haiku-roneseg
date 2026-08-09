// One probe, one process, run once on a freshly booted machine.
//
// WHY THIS REPLACES THE SEPARATE PROBES
//
// probe_usb / probe_at / probe_iso / probe_desc each opened the device, and
// each left at least one thread parked inside the USB driver on a transfer
// that never completed. Those threads do not go away when the process is
// killed - kill -9 leaves them, confirmed - so every probe after the first
// ran against a device that already had blocked transfers outstanding
// against it. Only the very first run of probe_usb was a clean measurement;
// everything after it has to be treated as contaminated, including the
// isochronous result and the total absence of control-transfer replies.
//
// So: one process, one pass, cheapest and most informative question first,
// and nothing re-run afterwards.
//
//   1. Control transfers. Every USB device answers these during enumeration,
//      so a device that does not answer them now is in a different state
//      than it was at boot - which is itself the answer to why it will not
//      stream. Also where the Microsoft OS descriptor lives, which would
//      identify the matching driver and so the chip.
//   2. Bulk IN on every alternate setting.
//   3. Isochronous IN, which is how a tuner normally delivers a transport
//      stream and which the EHCI fixes in the VAIO P patch set made usable.
//
// Passive throughout: standard IN requests and reads. The only vendor-coded
// request is the one the device itself publishes in its MS OS descriptor,
// and only if it publishes one.
//
// Build:  setarch x86 g++ -o probe_all probe_all.cpp -ldevice
// Run:    ./probe_all /dev/bus/usb/3/2 2>&1 | tee probe_all.log

#include <USBKit.h>
#include <OS.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const bigtime_t kControlTimeout = 1500000;
static const bigtime_t kStreamTimeout = 3000000;
static const uint8 kGetDescriptor = 0x06;
static const uint16 kStringType = 0x0300;


// #pragma mark - timed transfers


struct Job {
	BUSBDevice*					device;
	const BUSBEndpoint*			endpoint;
	uint8						requestType;
	uint8						request;
	uint16						value;
	uint16						index;
	uint16						length;
	uint8*						buffer;
	size_t						size;
	usb_iso_packet_descriptor*	packets;
	uint32						packetCount;
	ssize_t						result;
	sem_id						done;
};


static status_t
ControlEntry(void* cookie)
{
	Job* job = (Job*)cookie;
	job->result = job->device->ControlTransfer(job->requestType, job->request,
		job->value, job->index, job->length, job->buffer);
	release_sem(job->done);
	return B_OK;
}


static status_t
BulkEntry(void* cookie)
{
	Job* job = (Job*)cookie;
	job->result = job->endpoint->BulkTransfer(job->buffer, job->size);
	release_sem(job->done);
	return B_OK;
}


static status_t
IsoEntry(void* cookie)
{
	Job* job = (Job*)cookie;
	job->result = job->endpoint->IsochronousTransfer(job->buffer, job->size,
		job->packets, job->packetCount);
	release_sem(job->done);
	return B_OK;
}


// Returns the transfer result, or -1 on timeout. Every timeout leaks one
// parked thread, which is why this program is meant to be run once.
static ssize_t
Run(status_t (*entry)(void*), Job& job, bigtime_t timeout)
{
	job.result = 0;
	job.done = create_sem(0, "probe-done");
	thread_id thread = spawn_thread(entry, "probe", B_NORMAL_PRIORITY, &job);
	if (thread < 0) {
		delete_sem(job.done);
		return -1;
	}
	resume_thread(thread);

	status_t waited = acquire_sem_etc(job.done, 1, B_RELATIVE_TIMEOUT,
		timeout);
	delete_sem(job.done);
	return waited == B_OK ? job.result : -1;
}


// #pragma mark - helpers


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


static void
PrintString(const uint8* data, ssize_t size)
{
	printf("\"");
	for (ssize_t i = 2; i + 1 < size; i += 2) {
		uint16 c = data[i] | (data[i + 1] << 8);
		if (c >= 0x20 && c < 0x7f)
			printf("%c", (char)c);
		else if (c != 0)
			printf("\\u%04x", c);
	}
	printf("\"");
}


static void
DumpHex(const uint8* data, ssize_t size)
{
	for (ssize_t i = 0; i < size && i < 64; i++) {
		printf("%02x ", data[i]);
		if (i % 16 == 15)
			printf("\n      ");
	}
	printf("\n");
}


// #pragma mark - main


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

	printf("%s  %04x:%04x  class 0x%02x  USB %04x\n\n", path,
		device.VendorID(), device.ProductID(), device.Class(),
		device.USBVersion());

	uint8 buffer[4096];
	Job job;
	job.device = &device;
	job.buffer = buffer;

	bool controlAnswers = false;

	// #pragma mark 1 - control


	printf("== 1. control transfers ==\n");

	printf("language IDs (string 0): ");
	memset(buffer, 0, sizeof(buffer));
	job.requestType = 0x80;
	job.request = kGetDescriptor;
	job.value = kStringType | 0;
	job.index = 0;
	job.length = 255;
	ssize_t read = Run(ControlEntry, job, kControlTimeout);
	uint16 language = 0x0409;
	if (read < 0) {
		printf("timed out\n");
	} else if (read < 4) {
		printf("%" B_PRIdSSIZE " bytes (device has no strings)\n", read);
		controlAnswers = true;
	} else {
		language = buffer[2] | (buffer[3] << 8);
		printf("0x%04x\n", language);
		controlAnswers = true;
	}

	// A device with no strings still answers this; a device that is not
	// listening does not. That distinction is the point of asking.
	printf("device descriptor re-read: ");
	memset(buffer, 0, sizeof(buffer));
	job.requestType = 0x80;
	job.request = kGetDescriptor;
	job.value = 0x0100;
	job.index = 0;
	job.length = 18;
	read = Run(ControlEntry, job, kControlTimeout);
	if (read < 0) {
		printf("timed out\n");
	} else {
		printf("%" B_PRIdSSIZE " bytes\n", read);
		if (read >= 18) {
			printf("      ");
			DumpHex(buffer, read);
			controlAnswers = true;
		}
	}

	if (controlAnswers) {
		printf("string indices: ");
		int found = 0;
		for (uint16 index = 1; index <= 6; index++) {
			memset(buffer, 0, sizeof(buffer));
			job.requestType = 0x80;
			job.request = kGetDescriptor;
			job.value = kStringType | index;
			job.index = language;
			job.length = 255;
			read = Run(ControlEntry, job, kControlTimeout);
			if (read >= 4) {
				printf("\n  [%u] ", index);
				PrintString(buffer, read);
				found++;
			}
		}
		printf(found > 0 ? "\n" : "none\n");

		printf("Microsoft OS descriptor (0xEE): ");
		memset(buffer, 0, sizeof(buffer));
		job.requestType = 0x80;
		job.request = kGetDescriptor;
		job.value = kStringType | 0xEE;
		job.index = 0;
		job.length = 0x12;
		read = Run(ControlEntry, job, kControlTimeout);
		if (read < 0x12) {
			printf("not present (%" B_PRIdSSIZE ")\n", read);
		} else {
			bool isMsft = buffer[2] == 'M' && buffer[4] == 'S'
				&& buffer[6] == 'F' && buffer[8] == 'T' && buffer[10] == '1';
			if (!isMsft) {
				printf("answered, signature not MSFT100\n");
				printf("      ");
				DumpHex(buffer, read);
			} else {
				uint8 vendorCode = buffer[16];
				printf("MSFT100, vendor code 0x%02x\n", vendorCode);

				memset(buffer, 0, sizeof(buffer));
				job.requestType = 0xC0;
				job.request = vendorCode;
				job.value = 0x0000;
				job.index = 0x0004;
				job.length = 0x28;
				read = Run(ControlEntry, job, kControlTimeout);
				printf("extended compat ID: %" B_PRIdSSIZE " bytes\n", read);
				if (read >= 40) {
					printf("      ");
					DumpHex(buffer, read);
					char compatible[9];
					memcpy(compatible, buffer + 18, 8);
					compatible[8] = '\0';
					printf("  >>> compatible ID: \"%s\"\n", compatible);
					printf("  >>> that names the matching driver, and so the "
						"chip.\n");
				}
			}
		}
	} else {
		printf("\nThe control endpoint is not answering at all.\n"
			"It must have answered during enumeration - listusb shows this\n"
			"device's descriptors - so the module has gone unresponsive\n"
			"since boot rather than never having worked. That points at\n"
			"power or reset state, not at a missing driver.\n");
	}

	// #pragma mark 2 and 3 - streaming


	const BUSBConfiguration* config = device.ActiveConfiguration();
	if (config == NULL)
		config = device.ConfigurationAt(0);
	if (config == NULL) {
		printf("\nno configuration; stopping\n");
		return 0;
	}

	BUSBInterface* interface = const_cast<BUSBInterface*>(
		config->InterfaceAt(0));
	if (interface == NULL) {
		printf("\nno interface 0; stopping\n");
		return 0;
	}

	bool sawStream = false;

	printf("\n== 2/3. streaming endpoints ==\n");

	for (uint32 a = 1; a < interface->CountAlternates(); a++) {
		printf("\nalternate %" B_PRIu32 ":\n", a);
		status_t status = interface->SetAlternate(a);
		if (status != B_OK) {
			printf("  SetAlternate failed: %s\n", strerror(status));
			continue;
		}

		for (uint32 e = 0; e < interface->CountEndpoints(); e++) {
			const BUSBEndpoint* endpoint = interface->EndpointAt(e);
			if (endpoint == NULL || !endpoint->IsInput())
				continue;

			uint8 address = endpoint->Descriptor()->endpoint_address;

			if (endpoint->IsBulk()) {
				printf("  endpoint 0x%02x bulk IN: ", address);
				memset(buffer, 0, sizeof(buffer));
				job.endpoint = endpoint;
				job.size = sizeof(buffer);
				read = Run(BulkEntry, job, kStreamTimeout);
				if (read < 0) {
					printf("no data\n");
					continue;
				}
				printf("%" B_PRIdSSIZE " bytes\n", read);
				if (read > 0) {
					int packet = DetectTransportStream(buffer, read);
					if (packet > 0) {
						printf("    >>> MPEG-2 TS, %d-byte packets. This is "
							"the tuner.\n", packet);
						sawStream = true;
					} else {
						printf("      ");
						DumpHex(buffer, read);
					}
				}
				continue;
			}

			if (!endpoint->IsIsochronous())
				continue;

			// Real per-transaction size: bits 12:11 of wMaxPacketSize hold
			// transactions-per-microframe for a high-bandwidth endpoint, so
			// the usable bytes are the base times that count. Getting this
			// wrong is what made the webcam produce nothing before the patch
			// set fixed the same arithmetic in UVCCamDevice.
			uint16 raw = endpoint->Descriptor()->max_packet_size;
			uint32 base = raw & 0x07ff;
			uint32 perMicroframe = ((raw >> 11) & 0x03) + 1;
			size_t packetSize = base * perMicroframe;
			uint32 packetCount = 32;

			printf("  endpoint 0x%02x iso IN, %zu bytes/packet: ", address,
				packetSize);

			usb_iso_packet_descriptor packets[64];
			memset(packets, 0, sizeof(packets));
			for (uint32 p = 0; p < packetCount; p++)
				packets[p].request_length = (int16)packetSize;

			memset(buffer, 0, sizeof(buffer));
			job.endpoint = endpoint;
			job.size = packetSize * packetCount;
			if (job.size > sizeof(buffer))
				job.size = sizeof(buffer);
			job.packets = packets;
			job.packetCount = packetCount;
			read = Run(IsoEntry, job, kStreamTimeout);
			if (read < 0) {
				printf("no data\n");
				continue;
			}

			size_t total = 0;
			int nonEmpty = 0;
			for (uint32 p = 0; p < packetCount; p++) {
				if (packets[p].actual_length > 0) {
					nonEmpty++;
					total += packets[p].actual_length;
				}
			}
			printf("returned %" B_PRIdSSIZE ", %d/%" B_PRIu32 " packets, "
				"%zu bytes\n", read, nonEmpty, packetCount, total);

			if (total > 0) {
				// Compact before looking for sync bytes: the DMA buffer is
				// laid out at packetSize stride whatever each packet got, so
				// scanning it raw would see padding and miss the rhythm.
				uint8* packed = (uint8*)malloc(total);
				size_t offset = 0;
				for (uint32 p = 0; p < packetCount; p++) {
					if (packets[p].actual_length <= 0)
						continue;
					memcpy(packed + offset, buffer + p * packetSize,
						packets[p].actual_length);
					offset += packets[p].actual_length;
				}
				int packet = DetectTransportStream(packed, total);
				if (packet > 0) {
					printf("    >>> MPEG-2 TS, %d-byte packets. This is the "
						"tuner.\n", packet);
					sawStream = true;
					FILE* out = fopen("/tmp/oneseg-capture.ts", "wb");
					if (out != NULL) {
						fwrite(packed, 1, total, out);
						fclose(out);
						printf("    >>> saved to /tmp/oneseg-capture.ts\n");
					}
				} else {
					printf("      ");
					DumpHex(packed, total);
				}
				free(packed);
			}
		}
	}

	interface->SetAlternate(0);

	printf("\n== verdict ==\n");
	if (sawStream) {
		printf("Transport stream seen. This is the tuner and that is the "
			"pipe.\n");
	} else if (!controlAnswers) {
		printf("Not one transfer of any kind answered, control included.\n"
			"The module is present on the bus but not responding, so no\n"
			"device profile could help until that is resolved.\n");
	} else {
		printf("Control works, streaming does not: the module is listening\n"
			"but has not been told to start. That is the missing device\n"
			"profile - an initialisation sequence and probably a firmware\n"
			"upload.\n");
	}

	printf("\nNOTE: any timeout above left a thread parked in the driver.\n"
		"Reboot before probing again, or the next run measures this one.\n");
	return 0;
}
