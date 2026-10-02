// Scriptable access to the One-Seg module's register bus, for experiments
// against a live signal. Reads commands from stdin, one per line, all numbers
// hex:
//
//   w SUB REG VAL...     write VAL to REG, REG+1, ... of sub-device SUB (0x20)
//   wb SUB REG VAL...    one burst write: REG then all VALs in one transfer
//   r SUB REG N          read N registers (0x21, wValue = N)
//   mode                 set mode 1 (0x23)
//   fifo                 FIFO reset (0x27)
//   ctl TYPE REQ VALUE INDEX LEN [BYTES...]   any control transfer
//   bulk SIZE MS         one bulk read of the stream endpoint (decimal)
//   sleep MS             (decimal)
//   # ...                comment
//
// Every transfer runs on its own thread with a deadline (see AGENTS.md §4).
//
// Build:  setarch x86 g++ -o oneseg_cmd tools/oneseg_cmd.cpp -ldevice -lbe

#include <USBKit.h>
#include <OS.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BUSBDevice* sDevice = NULL;


class Roster : public BUSBRoster {
public:
	virtual status_t DeviceAdded(BUSBDevice* device)
	{
		if (sDevice == NULL && device->VendorID() == 0x054c
			&& device->ProductID() == 0x0279) {
			sDevice = device;
			return B_OK;
		}
		return B_ERROR;
	}
	virtual void DeviceRemoved(BUSBDevice* device)
	{
		if (device == sDevice)
			sDevice = NULL;
	}
};


struct Job {
	bool		bulk;
	const BUSBEndpoint* endpoint;
	uint8		type, request;
	uint16		value, index;
	uint32		length;
	uint8*		buffer;
	ssize_t		result;
	sem_id		done;
};


static status_t
JobThread(void* cookie)
{
	Job* job = (Job*)cookie;
	if (job->bulk)
		job->result = job->endpoint->BulkTransfer(job->buffer, job->length);
	else
		job->result = sDevice->ControlTransfer(job->type, job->request,
			job->value, job->index, job->length, job->buffer);
	release_sem(job->done);
	return B_OK;
}


static ssize_t
Run(Job* job, bigtime_t timeout)
{
	job->result = 0;
	job->done = create_sem(0, "cmd");
	thread_id thread = spawn_thread(JobThread, "oneseg cmd", B_NORMAL_PRIORITY,
		job);
	resume_thread(thread);
	status_t waited = acquire_sem_etc(job->done, 1, B_RELATIVE_TIMEOUT, timeout);
	if (waited != B_OK) {
		// usb_raw cancels a transfer whose thread is killed, which releases
		// its per-device lock; the job is on our stack, so wait for the end.
		kill_thread(thread);
		status_t ignored;
		wait_for_thread(thread, &ignored);
	}
	delete_sem(job->done);
	return waited == B_OK ? job->result : B_TIMED_OUT;
}


static ssize_t
Control(uint8 type, uint8 request, uint16 value, uint16 index, uint16 length,
	uint8* buffer)
{
	uint8 scratch[2] = { 0, 0 };
	Job job;
	job.bulk = false;
	job.type = type;
	job.request = request;
	job.value = value;
	job.index = index;
	job.length = length;
	job.buffer = buffer != NULL ? buffer : scratch;
	return Run(&job, 1000000);
}


static const BUSBEndpoint*
StreamEndpoint()
{
	const BUSBConfiguration* config = sDevice->ActiveConfiguration();
	if (config == NULL)
		return NULL;
	const BUSBInterface* interface = config->InterfaceAt(0);
	if (interface == NULL)
		return NULL;
	for (uint32 e = 0; e < interface->CountEndpoints(); e++) {
		const BUSBEndpoint* endpoint = interface->EndpointAt(e);
		if (endpoint->IsBulk() && endpoint->IsInput()
			&& endpoint->MaxPacketSize() > 64)
			return endpoint;
	}
	return NULL;
}


static bool
LooksLikeTS(const uint8* data, ssize_t size)
{
	for (ssize_t start = 0; start < 188 && start < size; start++) {
		int hits = 0;
		for (ssize_t off = start; off < size && data[off] == 0x47; off += 188)
			hits++;
		if (hits >= 5)
			return true;
	}
	return false;
}


int
main()
{
	Roster roster;
	roster.Start();
	for (int i = 0; i < 30 && sDevice == NULL; i++)
		snooze(100000);
	if (sDevice == NULL) {
		printf("no 054c:0279\n");
		return 1;
	}
	printf("device bcd %04x\n", sDevice->Descriptor()->device_version);
	setvbuf(stdout, NULL, _IOLBF, 0);

	char line[1024];
	while (fgets(line, sizeof(line), stdin) != NULL) {
		char* save = NULL;
		char* word = strtok_r(line, " \t\r\n", &save);
		if (word == NULL || word[0] == '#')
			continue;
		unsigned long args[300];
		int count = 0;
		char* token;
		while (count < 300
			&& (token = strtok_r(NULL, " \t\r\n", &save)) != NULL)
			args[count++] = strtoul(token, NULL,
				strcmp(word, "sleep") == 0 || strcmp(word, "bulk") == 0
					|| strcmp(word, "fail") == 0 || strcmp(word, "agc") == 0
					|| strcmp(word, "gain") == 0
					? 10 : 16);

		if (strcmp(word, "w") == 0 && count >= 3) {
			for (int i = 2; i < count; i++) {
				uint8 byte = args[i];
				ssize_t got = Control(0x40, 0x20, 1,
					(args[0] << 8) | (args[1] + i - 2), 1, &byte);
				if (got != 1)
					printf("w %02lx %02lx: %s\n", args[0], args[1] + i - 2,
						strerror(got));
			}
		} else if (strcmp(word, "wb") == 0 && count >= 3) {
			// One transfer: REG then every byte, as a single I2C write.
			uint8 bytes[256];
			for (int i = 2; i < count; i++)
				bytes[i - 2] = args[i];
			ssize_t got = Control(0x40, 0x20, count - 2,
				(args[0] << 8) | args[1], count - 2, bytes);
			if (got != count - 2)
				printf("wb %02lx %02lx: %s\n", args[0], args[1], strerror(got));
		} else if (strcmp(word, "r") == 0 && count == 3) {
			uint8 buffer[64];
			ssize_t got = Control(0xc0, 0x21, args[2],
				(args[0] << 8) | args[1], args[2], buffer);
			if (got < 0) {
				printf("r %02lx %02lx: %s\n", args[0], args[1], strerror(got));
				continue;
			}
			printf("r %02lx %02lx:", args[0], args[1]);
			for (ssize_t i = 0; i < got; i++)
				printf(" %02x", buffer[i]);
			printf("\n");
		} else if (strcmp(word, "mode") == 0) {
			printf("mode: %ld\n", Control(0x40, 0x23, 1, 0, 0, NULL));
		} else if (strcmp(word, "fifo") == 0) {
			printf("fifo: %ld\n", Control(0x40, 0x27, 0, 0, 0, NULL));
		} else if (strcmp(word, "ctl") == 0 && count >= 5) {
			uint8 buffer[256];
			memset(buffer, 0, sizeof(buffer));
			for (int i = 5; i < count; i++)
				buffer[i - 5] = args[i];
			ssize_t got = Control(args[0], args[1], args[2], args[3], args[4],
				buffer);
			printf("ctl -> %ld:", got);
			for (ssize_t i = 0; (args[0] & 0x80) && i < got; i++)
				printf(" %02x", buffer[i]);
			printf("\n");
		} else if (strcmp(word, "bulk") == 0 && count == 2) {
			// Never zero: a zero-length bulk transfer panics Haiku's EHCI
			// driver (CreateDescriptorChain builds no descriptor and
			// FillQueueWithData dereferences it).
			if (args[0] == 0) {
				printf("bulk: zero length refused\n");
				continue;
			}
			const BUSBEndpoint* endpoint = StreamEndpoint();
			if (endpoint == NULL) {
				printf("no stream endpoint\n");
				continue;
			}
			static uint8 buffer[65536];
			Job job;
			job.bulk = true;
			job.endpoint = endpoint;
			job.length = args[0] > sizeof(buffer) ? sizeof(buffer) : args[0];
			job.buffer = buffer;
			ssize_t got = Run(&job, args[1] * 1000);
			printf("bulk: %ld%s", got, got > 0 && LooksLikeTS(buffer, got)
				? " TS" : "");
			for (ssize_t i = 0; i < got && i < 32; i++)
				printf(" %02x", buffer[i]);
			printf("\n");
		} else if (strcmp(word, "fail") == 0 && count == 1) {
			// Poll demodulator 0x00 until bit 3 (gave up) or bit 1 with
			// 0x02 bit 3 (lock); report how long it took. Decimal ms.
			bigtime_t start = system_time();
			const char* verdict = "timeout";
			uint8 r[3] = { 0 };
			while (system_time() - start < (bigtime_t)args[0] * 1000) {
				if (Control(0xc0, 0x21, 3, 0x6e00, 3, r) != 3)
					continue;
				if ((r[0] & 0x02) && (r[2] & 0x08)) { verdict = "LOCK"; break; }
				if (r[0] & 0x08) { verdict = "fail"; break; }
			}
			printf("fail: %s %lld ms (%02x %02x %02x)\n", verdict,
				(system_time() - start) / 1000, r[0], r[1], r[2]);
		} else if (strcmp(word, "agc") == 0 && count == 1) {
			// Average demodulator 0x11 and tuner-side 0x18/0x19 spread over
			// N reads (decimal).
			double sum = 0, sum2 = 0;
			int n = 0;
			for (unsigned long i = 0; i < args[0]; i++) {
				uint8 r[16];
				if (Control(0xc0, 0x21, 16, 0x6e10, 16, r) != 16)
					continue;
				sum += r[1];
				sum2 += (r[8] - 128.0) * (r[8] - 128.0) + (r[9] - 128.0) * (r[9] - 128.0);
				n++;
			}
			printf("agc: %.2f  spread %.1f  (n=%d)\n", n ? sum / n : -1,
				n ? sum2 / n : -1, n);
		} else if (strcmp(word, "gain") == 0 && count == 1) {
			// Mean of demodulator 0x0f (AGC gain, 0x96 = maximum) and 0x11
			// (level after the AGC) over N reads (decimal).
			double gain = 0, level = 0;
			int n = 0;
			for (unsigned long i = 0; i < args[0]; i++) {
				uint8 r[3];
				if (Control(0xc0, 0x21, 3, 0x6e0f, 3, r) != 3)
					continue;
				gain += r[0];
				level += r[2];
				n++;
			}
			printf("gain: %.3f level %.2f (n=%d)\n", n ? gain / n : -1,
				n ? level / n : -1, n);
		} else if (strcmp(word, "sleep") == 0 && count == 1) {
			snooze(args[0] * 1000);
		} else {
			printf("? %s\n", word);
		}
	}
	roster.Stop();
	return 0;
}
