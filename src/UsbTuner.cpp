#include "UsbTuner.h"

#include <Autolock.h>
#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Message.h>
#include <OS.h>
#include <Path.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The One-Seg module, driven end to end from userland - no kernel driver, just
// the USB Kit's control and bulk transfers.
//
//   * The module is a Cypress EZ-USB, 054c:0279, blank on power-up (bcdDevice
//     0). It takes an 8051 firmware image and renumerates as "CXD9192
//     Controller" with a large bulk IN endpoint carrying the transport stream.
//   * Vendor requests: 0x20 writes a register (wIndex = (sub<<8)|reg,
//     wValue = value byte), 0x23 sets the mode, 0x27 resets the FIFOs. The
//     firmware relays register access, so the host writes every register value.
//   * Bring-up is two fixed sequences: the demodulator (sub 0x6E) and the RF
//     front end (sub 0x6C table + sub 0x60 tuner). Neither depends on channel.
//   * A channel is the frequency word V = 7 x f_MHz, written to demod 0x32/0x33
//     and latched with a 0x42 pulse. If a signal is present and no channel
//     locks, this select register is the first thing to adjust - see Tune().
//
// The exact lock register is not identified; the scanner does not need it. It
// asks the only question that matters - "are TS packets coming out on this
// channel" - by reading the data endpoint and looking for sync bytes. Once a
// stream flows, the rest of the app (demux, SI, decode) needs nothing from
// here: it scans, names channels from the SDT, and plays.

namespace {

const UsbTuner::DeviceProfile kProfiles[] = { };
const size_t kProfileCount = sizeof(kProfiles) / sizeof(kProfiles[0]);

const uint16 kVendor = 0x054C;
const uint16 kProduct = 0x0279;

// Vendor request bytes and the recipient/direction masks.
const uint8 kVendorOut = 0x40;
const uint8 kFirmwareLoad = 0xA0;			// EZ-USB bootloader firmware download
const uint16 kCpuCsRegister = 0xE600;
const uint8 kRegisterWrite = 0x20;
const uint8 kSetMode = 0x23;
const uint8 kFifoReset = 0x27;

// I2C sub-devices behind the bridge.
const uint8 kDemod = 0x6E;
const uint8 kFront = 0x6C;
const uint8 kTuner = 0x60;

// Demodulator init: a reset/latch pulse then a 35-register configuration to
// sub-device 0x6E. It leaves registers 0x32-0x35 at zero - the frequency word.
const uint8 kDemodLatch[][2] = {
	{ 0x42, 0x01 }, { 0x42, 0x00 }, { 0x41, 0x01 },
};
const uint8 kDemodInit[][2] = {
	{ 0x32, 0x00 }, { 0x33, 0x00 }, { 0x34, 0x00 }, { 0x35, 0x00 }, { 0x40, 0x00 },
	{ 0x43, 0x0d }, { 0x44, 0x22 }, { 0x46, 0x01 }, { 0x47, 0x14 }, { 0x48, 0x00 },
	{ 0x49, 0x01 }, { 0x4b, 0x16 }, { 0x4c, 0x80 }, { 0x4d, 0x8f }, { 0x4e, 0x82 },
	{ 0x4f, 0x08 }, { 0x50, 0x24 }, { 0x51, 0x8b }, { 0x52, 0x01 }, { 0x53, 0x01 },
	{ 0x56, 0x84 }, { 0x57, 0x20 }, { 0x59, 0xff }, { 0x5a, 0x69 }, { 0x5b, 0x12 },
	{ 0x5d, 0x40 }, { 0x5e, 0x90 }, { 0x5f, 0xff }, { 0x60, 0x00 }, { 0x62, 0x20 },
	{ 0x64, 0x00 }, { 0x65, 0x10 }, { 0x66, 0x00 }, { 0x67, 0x00 }, { 0x68, 0x83 },
};
// RF front-end init: a table to sub-device 0x6C and the tuner IC program to
// sub-device 0x60. Fixed - none of it depends on the channel.
const uint8 kFront6C[][2] = {
	{ 0x00, 0x20 }, { 0x4a, 0x2a }, { 0x84, 0x07 }, { 0x1b, 0x73 }, { 0x1c, 0x95 },
	{ 0x0b, 0x00 }, { 0x0c, 0x31 }, { 0x0d, 0x35 }, { 0x98, 0x80 }, { 0xdf, 0x01 },
	{ 0x05, 0x18 }, { 0x08, 0x32 }, { 0x61, 0x80 }, { 0x3c, 0x10 }, { 0x3d, 0x80 },
	{ 0xf2, 0x01 }, { 0xf4, 0x0a }, { 0xe7, 0x02 },
};
const uint8 kTuner60[][2] = {
	{ 0xa0, 0x54 }, { 0xa1, 0x02 }, { 0xa2, 0x67 }, { 0xa3, 0x00 }, { 0xa4, 0x06 },
	{ 0xa5, 0x3b }, { 0xa6, 0x00 }, { 0xa7, 0x56 }, { 0xa8, 0x00 }, { 0xa9, 0x5b },
	{ 0xaa, 0x02 }, { 0xab, 0x6e }, { 0xac, 0x02 }, { 0xad, 0x0b }, { 0xae, 0x01 },
	{ 0xaf, 0x42 }, { 0xb0, 0x01 }, { 0xb1, 0x0b }, { 0xb2, 0x00 }, { 0xb5, 0x00 },
};


// Where the frequency word might land, best-supported first. The first two are
// the two readings of DtvCore.dll (see AGENTS.md §6.4); the rest are their near
// neighbours - the registers the demodulator init table deliberately leaves at
// zero, paired both ways, with each latch value that appears in the vendor
// code. Short on purpose: this runs against one channel at a time, and a list
// long enough to be "thorough" would be too slow to sit through.
const UsbTuner::TuningCandidate kTuningCandidates[] = {
	{ 0x32, 0x33, 0x01 },		// the default
	{ 0x64, 0x67, 0x10 },		// DtvCore.dll 0x100905f7, latch 0x42 = 0x10
	{ 0x64, 0x67, 0x01 },
	{ 0x32, 0x33, 0x10 },
	{ 0x64, 0x65, 0x01 },
	{ 0x66, 0x67, 0x01 },
	{ 0x34, 0x35, 0x01 },
};


// ~/config/settings/roneseg/settings, where the frequency-word layout is kept.
// The same directory the firmware image lives in.
status_t
SettingsPath(BPath* path, bool createDirectory)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, path);
	if (status != B_OK)
		return status;
	status = path->Append("roneseg");
	if (status != B_OK)
		return status;
	if (createDirectory)
		create_directory(path->Path(), 0755);
	return path->Append("settings");
}


const char* kSettingHigh = "frequency register high";
const char* kSettingLow = "frequency register low";
const char* kSettingLatch = "latch value";


std::string
Hex16(uint16 value)
{
	char buffer[8];
	snprintf(buffer, sizeof(buffer), "%04x", value);
	return std::string(buffer);
}


const char*
ClassName(uint8 baseClass)
{
	switch (baseClass) {
		case 0x01: return "audio";
		case 0x02: return "communications";
		case 0x03: return "HID";
		case 0x08: return "mass storage";
		case 0x09: return "hub";
		case 0x0e: return "video";
		case 0xe0: return "wireless";
		case 0xef: return "miscellaneous";
		case 0xff: return "vendor specific";
		default:   return "other";
	}
}


// Is this a valid MPEG-2 transport stream? Sync bytes at 188-byte stride are
// not something arbitrary binary produces.
bool
LooksLikeTransportStream(const uint8* data, size_t size)
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
				return true;
		}
	}
	return false;
}


// Retains the one module matching 054c:0279 - in either its bootloader or its
// firmware identity - and lets it go again when it renumerates or unplugs.
// fDevice is shared with the owning UsbTuner and guarded by its lock.
class FindRoster : public BUSBRoster {
public:
	FindRoster(BUSBDevice** out, BLocker* lock)
		: fOut(out), fLock(lock) {}

	virtual status_t DeviceAdded(BUSBDevice* device)
	{
		if (device != NULL && device->VendorID() == kVendor
			&& device->ProductID() == kProduct) {
			BAutolock lock(fLock);
			if (*fOut == NULL) {
				*fOut = device;
				return B_OK;			// tell the roster to keep it alive
			}
		}
		return B_ERROR;
	}

	virtual void DeviceRemoved(BUSBDevice* device)
	{
		BAutolock lock(fLock);
		if (*fOut == device)
			*fOut = NULL;
	}

private:
	BUSBDevice**	fOut;
	BLocker*		fLock;
};


// A bulk read on its own timed thread: a bulk transfer with nothing to deliver
// never returns on its own, and a plain wait would hang the caller forever.
struct BulkJob {
	const BUSBEndpoint*	endpoint;
	void*				buffer;
	size_t				size;
	ssize_t				result;
	sem_id				done;
};


status_t
BulkThread(void* cookie)
{
	BulkJob* job = (BulkJob*)cookie;
	job->result = job->endpoint->BulkTransfer(job->buffer, job->size);
	release_sem(job->done);
	return B_OK;
}


// The same trick for control transfers: run it on a thread so a wedged device
// costs a timeout rather than an unkillable hang.
struct ControlJob {
	BUSBDevice*	device;
	uint8		requestType;
	uint8		request;
	uint16		value;
	uint16		index;
	uint16		length;
	void*		buffer;
	ssize_t		result;
	sem_id		done;
};


status_t
ControlThread(void* cookie)
{
	ControlJob* job = (ControlJob*)cookie;
	job->result = job->device->ControlTransfer(job->requestType, job->request,
		job->value, job->index, job->length, job->buffer);
	release_sem(job->done);
	return B_OK;
}

} // namespace


UsbTuner::UsbTuner()
	:
	fRoster(NULL),
	fDevice(NULL),
	fDeviceLock("roneseg usb device"),
	fInterface(NULL),
	fStreamEndpoint(NULL),
	fStatusEndpoint(NULL),
	fProfile(NULL),
	fReady(false),
	fFrequency(0),
	fFrequencyReg(0x32),
	fFrequencyRegLow(0x33),
	fLatchValue(0x01)
{
}


UsbTuner::~UsbTuner()
{
	Close();
}


const UsbTuner::DeviceProfile*
UsbTuner::ProfileFor(uint16 vendor, uint16 product)
{
	for (size_t i = 0; i < kProfileCount; i++) {
		if (kProfiles[i].vendorId == vendor
			&& kProfiles[i].productId == product) {
			return &kProfiles[i];
		}
	}
	return NULL;
}


status_t
UsbTuner::LoadSettings()
{
	BPath path;
	status_t status = SettingsPath(&path, false);
	if (status != B_OK)
		return status;

	BFile file(path.Path(), B_READ_ONLY);
	status = file.InitCheck();
	if (status != B_OK)
		return status;			// no settings yet: the defaults stand

	BMessage settings;
	status = settings.Unflatten(&file);
	if (status != B_OK)
		return status;

	// Each value is taken only if it is there and in range, so a truncated or
	// hand-edited file degrades to the compiled-in defaults rather than
	// putting the tuner somewhere impossible.
	int32 value;
	if (settings.FindInt32(kSettingHigh, &value) == B_OK
		&& value >= 0 && value <= 0xFF) {
		fFrequencyReg = (uint8)value;
	}
	if (settings.FindInt32(kSettingLow, &value) == B_OK
		&& value >= 0 && value <= 0xFF) {
		fFrequencyRegLow = (uint8)value;
	}
	if (settings.FindInt32(kSettingLatch, &value) == B_OK
		&& value >= 0 && value <= 0xFF) {
		fLatchValue = (uint8)value;
	}
	return B_OK;
}


status_t
UsbTuner::SaveSettings() const
{
	BPath path;
	status_t status = SettingsPath(&path, true);
	if (status != B_OK)
		return status;

	BMessage settings;
	settings.AddInt32(kSettingHigh, fFrequencyReg);
	settings.AddInt32(kSettingLow, fFrequencyRegLow);
	settings.AddInt32(kSettingLatch, fLatchValue);

	BFile file(path.Path(), B_CREATE_FILE | B_ERASE_FILE | B_WRITE_ONLY);
	status = file.InitCheck();
	if (status != B_OK)
		return status;
	return settings.Flatten(&file);
}


const UsbTuner::TuningCandidate*
UsbTuner::TuningCandidates(size_t* count)
{
	if (count != NULL)
		*count = sizeof(kTuningCandidates) / sizeof(kTuningCandidates[0]);
	return kTuningCandidates;
}


bool
UsbTuner::LooksLikeTuner(const BUSBDevice& device)
{
	uint8 baseClass = device.Class();
	if (baseClass == 0x09 || baseClass == 0x03 || baseClass == 0x0e
		|| baseClass == 0xe0) {
		return false;
	}

	for (uint32 c = 0; c < device.CountConfigurations(); c++) {
		const BUSBConfiguration* config = device.ConfigurationAt(c);
		if (config == NULL)
			continue;
		for (uint32 i = 0; i < config->CountInterfaces(); i++) {
			const BUSBInterface* interface = config->InterfaceAt(i);
			if (interface == NULL)
				continue;
			for (uint32 a = 0; a < interface->CountAlternates(); a++) {
				const BUSBInterface* alternate = interface->AlternateAt(a);
				if (alternate == NULL)
					continue;
				for (uint32 e = 0; e < alternate->CountEndpoints(); e++) {
					const BUSBEndpoint* endpoint = alternate->EndpointAt(e);
					if (endpoint != NULL && endpoint->IsBulk()
						&& endpoint->IsInput()) {
						return true;
					}
				}
			}
		}
	}
	// The blank bootloader answers control transfers but exposes no bulk IN
	// endpoint until its firmware runs, so match it by identity too.
	return device.VendorID() == kVendor && device.ProductID() == kProduct;
}


// #pragma mark - enumeration (unchanged: the report the app prints)


namespace {

class CollectingRoster : public BUSBRoster {
public:
	CollectingRoster(std::vector<UsbTuner::Candidate>* out) : fOut(out) {}

	virtual status_t DeviceAdded(BUSBDevice* device)
	{
		if (device == NULL)
			return B_ERROR;

		UsbTuner::Candidate candidate;
		candidate.vendorId = device->VendorID();
		candidate.productId = device->ProductID();
		candidate.manufacturer = device->ManufacturerString();
		candidate.product = device->ProductString();
		candidate.matchesProfile = false;

		char line[512];
		snprintf(line, sizeof(line),
			"  class 0x%02x (%s), subclass 0x%02x, protocol 0x%02x, "
			"USB %04x, %" B_PRIu32 " configuration(s)\n",
			device->Class(), ClassName(device->Class()), device->Subclass(),
			device->Protocol(), device->USBVersion(),
			device->CountConfigurations());
		candidate.detail = line;

		for (uint32 c = 0; c < device->CountConfigurations(); c++) {
			const BUSBConfiguration* config = device->ConfigurationAt(c);
			if (config == NULL)
				continue;
			for (uint32 i = 0; i < config->CountInterfaces(); i++) {
				const BUSBInterface* interface = config->InterfaceAt(i);
				if (interface == NULL)
					continue;
				for (uint32 a = 0; a < interface->CountAlternates(); a++) {
					const BUSBInterface* alt = interface->AlternateAt(a);
					if (alt == NULL)
						continue;
					snprintf(line, sizeof(line),
						"  config %" B_PRIu32 " interface %" B_PRIu32
						" alt %" B_PRIu32 ": class 0x%02x (%s), "
						"%" B_PRIu32 " endpoint(s)\n",
						c, i, a, alt->Class(), ClassName(alt->Class()),
						alt->CountEndpoints());
					candidate.detail += line;
					for (uint32 e = 0; e < alt->CountEndpoints(); e++) {
						const BUSBEndpoint* endpoint = alt->EndpointAt(e);
						if (endpoint == NULL)
							continue;
						const char* type = endpoint->IsBulk() ? "bulk"
							: endpoint->IsIsochronous() ? "isochronous"
							: endpoint->IsInterrupt() ? "interrupt" : "control";
						snprintf(line, sizeof(line),
							"    endpoint 0x%02x %s %s, max packet %"
							B_PRIu16 "\n",
							endpoint->Descriptor()->endpoint_address, type,
							endpoint->IsInput() ? "IN" : "OUT",
							endpoint->MaxPacketSize());
						candidate.detail += line;
					}
				}
			}
		}

		if (UsbTuner::LooksLikeTuner(*device))
			fOut->push_back(candidate);

		return B_ERROR;
	}

	virtual void DeviceRemoved(BUSBDevice* device) { (void)device; }

private:
	std::vector<UsbTuner::Candidate>* fOut;
};

} // namespace


std::vector<UsbTuner::Candidate>
UsbTuner::Scan()
{
	std::vector<Candidate> candidates;
	CollectingRoster roster(&candidates);
	roster.Start();
	roster.Stop();

	for (size_t i = 0; i < candidates.size(); i++) {
		candidates[i].matchesProfile
			= (candidates[i].vendorId == kVendor
				&& candidates[i].productId == kProduct)
			|| ProfileFor(candidates[i].vendorId, candidates[i].productId)
				!= NULL;
	}
	return candidates;
}


std::string
UsbTuner::ScanReport()
{
	std::vector<Candidate> candidates = Scan();

	std::string report;
	if (candidates.empty()) {
		report =
			"No USB device on this machine looks like a tuner.\n"
			"\n"
			"On a VAIO P the internal module sits on a UHCI companion, which\n"
			"enumerates nothing without the SCH USBLEGSUP fix from the VAIO P\n"
			"patch set - check you are running that ISO - and the module may\n"
			"only appear a few seconds after boot.\n";
		return report;
	}

	for (size_t i = 0; i < candidates.size(); i++) {
		const Candidate& candidate = candidates[i];
		report += "Device " + Hex16(candidate.vendorId) + ":"
			+ Hex16(candidate.productId);
		if (!candidate.manufacturer.empty() || !candidate.product.empty()) {
			report += "  " + candidate.manufacturer;
			if (!candidate.product.empty())
				report += " " + candidate.product;
		}
		report += candidate.matchesProfile
			? "   [the One-Seg module]\n" : "   [no profile]\n";
		report += candidate.detail;
		report += "\n";
	}

	report +=
		"054c:0279 is the One-Seg module. If it shows bcdDevice 0 and no bulk\n"
		"endpoint, it is a blank Cypress bootloader - R One-Seg uploads its\n"
		"firmware automatically on the first tune, after which it renumerates\n"
		"as \"CXD9192 Controller\" and streams.\n";
	return report;
}


// #pragma mark - the hardware path


std::string
UsbTuner::LocateFirmware() const
{
	const char* candidates[] = {
		NULL,		// filled from fFirmwarePath below
		"/boot/home/config/settings/roneseg/oneseg_fw.rec",
		"/boot/home/config/non-packaged/data/roneseg/oneseg_fw.rec",
		"/boot/home/fwtool/oneseg_fw.rec",
		"oneseg_fw.rec",
	};
	candidates[0] = fFirmwarePath.empty() ? NULL : fFirmwarePath.c_str();

	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		if (candidates[i] == NULL)
			continue;
		FILE* file = fopen(candidates[i], "rb");
		if (file != NULL) {
			fclose(file);
			return std::string(candidates[i]);
		}
	}
	return std::string();
}


status_t
UsbTuner::FindDevice(bigtime_t timeout)
{
	if (fRoster == NULL) {
		fRoster = new FindRoster(&fDevice, &fDeviceLock);
		fRoster->Start();		// enumerates already-attached devices inline
	}

	bigtime_t deadline = system_time() + timeout;
	while (true) {
		{
			BAutolock lock(&fDeviceLock);
			if (fDevice != NULL)
				return B_OK;
		}
		if (system_time() >= deadline)
			return B_DEVICE_NOT_FOUND;
		snooze(100000);
	}
}


status_t
UsbTuner::UploadFirmware()
{
	std::string path = LocateFirmware();
	if (path.empty()) {
		SetLastError("the module needs its firmware, but oneseg_fw.rec was not "
			"found - put it in ~/config/settings/roneseg/");
		return B_ENTRY_NOT_FOUND;
	}

	FILE* file = fopen(path.c_str(), "rb");
	if (file == NULL) {
		SetLastError("could not open the firmware file");
		return B_IO_ERROR;
	}

	{
		BAutolock lock(&fDeviceLock);
		if (fDevice == NULL) {
			fclose(file);
			return B_DEVICE_NOT_FOUND;
		}
	}

	// Halt the 8051, write every record, then release it. The device renumerates
	// on release, so nothing may touch it again afterwards.
	//
	// Every one of these goes through ControlTimed like the rest of the project:
	// this is the first thing a fresh module is asked to do, and a bootloader
	// that does not answer would otherwise park an unkillable driver thread
	// here - on the one path a user cannot avoid.
	uint8 reset = 0x01;
	ControlTimed(kVendorOut, kFirmwareLoad, kCpuCsRegister, 0, 1, &reset,
		1000000);

	uint8 buffer[64];
	while (true) {
		uint8 header[4];
		if (fread(header, 1, 4, file) != 4)
			break;
		uint16 address = header[0] | (header[1] << 8);
		uint16 length = header[2] | (header[3] << 8);
		if (length == 0 || length > sizeof(buffer))
			break;
		if (fread(buffer, 1, length, file) != length)
			break;
		if (ControlTimed(kVendorOut, kFirmwareLoad, address, 0, length, buffer,
				1000000) != (ssize_t)length) {
			// Leave the CPU running rather than halted, then report.
			uint8 run = 0x00;
			ControlTimed(kVendorOut, kFirmwareLoad, kCpuCsRegister, 0, 1, &run,
				1000000);
			fclose(file);
			SetLastError("firmware upload failed mid-way");
			return B_IO_ERROR;
		}
	}
	fclose(file);

	uint8 run = 0x00;
	ControlTimed(kVendorOut, kFirmwareLoad, kCpuCsRegister, 0, 1, &run, 1000000);

	// The bootloader identity is going away; drop our reference so FindDevice
	// waits for the firmware identity rather than returning the old one.
	{
		BAutolock lock(&fDeviceLock);
		fDevice = NULL;
	}
	snooze(2500000);				// give it time to renumerate

	status_t status = FindDevice(5000000);
	if (status != B_OK) {
		SetLastError("the module did not come back after its firmware upload");
		return status;
	}
	return B_OK;
}


status_t
UsbTuner::ClaimEndpoints()
{
	BAutolock lock(&fDeviceLock);
	if (fDevice == NULL)
		return B_DEVICE_NOT_FOUND;

	const BUSBConfiguration* config = fDevice->ActiveConfiguration();
	if (config == NULL) {
		config = fDevice->ConfigurationAt(0);
		if (config != NULL)
			fDevice->SetConfiguration(config);
	}
	if (config == NULL)
		return B_ERROR;

	fInterface = config->InterfaceAt(0);
	if (fInterface == NULL)
		return B_ERROR;

	fStreamEndpoint = NULL;
	fStatusEndpoint = NULL;
	for (uint32 e = 0; e < fInterface->CountEndpoints(); e++) {
		const BUSBEndpoint* endpoint = fInterface->EndpointAt(e);
		if (endpoint == NULL || !endpoint->IsBulk() || !endpoint->IsInput())
			continue;
		if (endpoint->MaxPacketSize() > 64)
			fStreamEndpoint = endpoint;		// the transport stream
		else
			fStatusEndpoint = endpoint;
	}

	if (fStreamEndpoint == NULL) {
		SetLastError("the module has no streaming endpoint - is the firmware "
			"running?");
		return B_ERROR;
	}
	return B_OK;
}


ssize_t
UsbTuner::ControlTimed(uint8 requestType, uint8 request, uint16 value,
	uint16 index, uint16 length, void* buffer, bigtime_t timeout)
{
	uint8 scratch = 0;
	ControlJob job;
	{
		BAutolock lock(&fDeviceLock);
		if (fDevice == NULL)
			return B_DEVICE_NOT_FOUND;
		job.device = fDevice;
	}
	job.requestType = requestType;
	job.request = request;
	job.value = value;
	job.index = index;
	job.length = length;
	job.buffer = buffer != NULL ? buffer : &scratch;
	job.result = 0;
	job.done = create_sem(0, "roneseg control");
	if (job.done < 0)
		return B_ERROR;

	thread_id thread = spawn_thread(ControlThread, "roneseg control",
		B_NORMAL_PRIORITY, &job);
	if (thread < 0) {
		delete_sem(job.done);
		return B_ERROR;
	}
	resume_thread(thread);

	status_t waited = acquire_sem_etc(job.done, 1, B_RELATIVE_TIMEOUT, timeout);
	delete_sem(job.done);
	if (waited != B_OK)
		return B_TIMED_OUT;			// a wedged transfer, not an unkillable hang
	return job.result;
}


bool
UsbTuner::WriteRegister(uint8 sub, uint8 reg, uint8 value)
{
	uint8 byte = value;
	return ControlTimed(kVendorOut, kRegisterWrite, 1,
		(uint16)((sub << 8) | reg), 1, &byte, 1000000) == 1;
}


status_t
UsbTuner::BringUp()
{
	if (ControlTimed(kVendorOut, kSetMode, 1, 0, 0, NULL, 1000000) < 0)
		return B_DEVICE_NOT_FOUND;

	for (size_t i = 0; i < sizeof(kDemodLatch) / 2; i++) {
		if (!WriteRegister(kDemod, kDemodLatch[i][0], kDemodLatch[i][1]))
			return B_IO_ERROR;
	}
	for (size_t i = 0; i < sizeof(kDemodInit) / 2; i++) {
		if (!WriteRegister(kDemod, kDemodInit[i][0], kDemodInit[i][1]))
			return B_IO_ERROR;
	}
	for (size_t i = 0; i < sizeof(kFront6C) / 2; i++) {
		if (!WriteRegister(kFront, kFront6C[i][0], kFront6C[i][1]))
			return B_IO_ERROR;
	}
	for (size_t i = 0; i < sizeof(kTuner60) / 2; i++) {
		if (!WriteRegister(kTuner, kTuner60[i][0], kTuner60[i][1]))
			return B_IO_ERROR;
	}
	return B_OK;
}


status_t
UsbTuner::Open()
{
	if (fReady)
		return B_OK;

	status_t status = FindDevice(3000000);
	if (status != B_OK) {
		SetLastError("no One-Seg module found (054c:0279) - press U for the "
			"USB report");
		return status;
	}

	uint16 version;
	{
		BAutolock lock(&fDeviceLock);
		version = fDevice != NULL
			? fDevice->Descriptor()->device_version : 0;
	}

	if (version == 0) {
		status = UploadFirmware();
		if (status != B_OK)
			return status;
	}

	status = ClaimEndpoints();
	if (status != B_OK)
		return status;

	status = BringUp();
	if (status != B_OK) {
		SetLastError("the module answered but its bring-up sequence failed");
		return status;
	}

	fReady = true;
	fDescription = "CXD9192 One-Seg (054c:0279)";
	return B_OK;
}


void
UsbTuner::Close()
{
	fReady = false;
	fStreamEndpoint = NULL;
	fStatusEndpoint = NULL;
	fInterface = NULL;
	fProfile = NULL;
	fFrequency = 0;
	// The roster owns fDevice; stopping it releases the reference.
	if (fRoster != NULL) {
		fRoster->Stop();
		delete fRoster;
		fRoster = NULL;
	}
	BAutolock lock(&fDeviceLock);
	fDevice = NULL;
}


status_t
UsbTuner::Tune(uint64 frequencyHz)
{
	if (!fReady) {
		status_t status = Open();
		if (status != B_OK)
			return status;
	}

	// V = 7 x f_MHz, the exact ISDB-T raster integer.
	double mhz = (double)frequencyHz / 1000000.0;
	int v = (int)lround(7.0 * mhz);

	// Frequency word, then the 0x42 latch pulse. Both registers and the latch
	// value are configurable (default 0x32/0x33, latch 0x01); if a signal is
	// present and nothing locks, this is the first thing to adjust - the other
	// candidate read out of DtvCore.dll is 0x64/0x67 with latch 0x10, which is
	// why the low register is not assumed to be the high one plus one.
	if (!WriteRegister(kDemod, fFrequencyReg, (uint8)(v >> 8))
		|| !WriteRegister(kDemod, fFrequencyRegLow, (uint8)(v & 0xFF))) {
		SetLastError("could not write the frequency");
		return B_IO_ERROR;
	}
	WriteRegister(kDemod, 0x42, fLatchValue);
	WriteRegister(kDemod, 0x42, 0x00);

	// Restart the FIFOs so the stream begins cleanly on the new channel.
	ControlTimed(kVendorOut, kFifoReset, 0, 0, 0, NULL, 1000000);

	fFrequency = frequencyHz;
	return B_OK;
}


ssize_t
UsbTuner::BulkRead(void* buffer, size_t size, bigtime_t timeout)
{
	const BUSBEndpoint* endpoint = fStreamEndpoint;
	if (endpoint == NULL)
		return B_NO_INIT;

	BulkJob job;
	job.endpoint = endpoint;
	job.buffer = buffer;
	job.size = size;
	job.result = 0;
	job.done = create_sem(0, "roneseg bulk");
	if (job.done < 0)
		return B_ERROR;

	thread_id thread = spawn_thread(BulkThread, "roneseg bulk",
		B_NORMAL_PRIORITY, &job);
	if (thread < 0) {
		delete_sem(job.done);
		return B_ERROR;
	}
	resume_thread(thread);

	status_t waited = acquire_sem_etc(job.done, 1, B_RELATIVE_TIMEOUT, timeout);
	delete_sem(job.done);
	if (waited != B_OK)
		return 0;					// timeout: no data, caller keeps going
	return job.result;
}


ssize_t
UsbTuner::Read(void* buffer, size_t size)
{
	if (!fReady)
		return B_NO_INIT;
	return BulkRead(buffer, size, 1000000);
}


bool
UsbTuner::HasSignal(uint64 frequencyHz, bigtime_t timeout)
{
	fDiagnostic = Diagnostic();
	if (Tune(frequencyHz) != B_OK)
		return false;
	fDiagnostic.tuned = true;
	snooze(500000);					// let the demod acquire and the FIFO fill

	const size_t kSize = 16384;
	uint8* buffer = (uint8*)malloc(kSize);
	if (buffer == NULL)
		return false;

	// One read per channel: a channel with no stream never answers, and each
	// unanswered bulk transfer costs time, so keep the scan quick and
	// responsive to cancellation by asking once.
	ssize_t got = BulkRead(buffer, kSize, timeout);
	bool stream = got > 0 && LooksLikeTransportStream(buffer, got);
	fDiagnostic.bytes = got;
	fDiagnostic.sync = stream;
	free(buffer);
	return stream;
}


status_t
UsbTuner::GetStatus(Status* out)
{
	if (out == NULL)
		return B_BAD_VALUE;
	if (!fReady)
		return B_NO_INIT;

	const size_t kSize = 8192;
	uint8* buffer = (uint8*)malloc(kSize);
	if (buffer == NULL)
		return B_NO_MEMORY;

	ssize_t got = BulkRead(buffer, kSize, 400000);
	out->locked = got > 0 && LooksLikeTransportStream(buffer, got);
	out->strength = -1;
	out->quality = -1;
	free(buffer);
	return B_OK;
}


std::string
UsbTuner::Description() const
{
	return fDescription.empty() ? std::string("(no tuner)") : fDescription;
}
