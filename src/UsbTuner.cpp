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
//   * Bring-up is the sequence Sony's DtvCore.dll performs for this module:
//     the demodulator (0x6E) takes a DSP program and a register table, and
//     the RF tuner behind its I2C repeater (0x63) an init stream.
//   * A channel is the tuner's PLL word, 0x7644 for UHF 13 and 0x180 more per
//     channel - see Tune().
//
// The demodulator reports its own verdict (WaitForLock): register 0x00 bit 1
// with 0x02 bit 3 is a lock, 0x00 bit 3 means it gave up. The scanner only
// reads the data endpoint once that says locked. Register 0x0f is the AGC
// gain (0x96 = maximum) and the one to watch for signal strength; 0x11 is
// the level after the AGC and reads the same whatever the input.

namespace {

const UsbTuner::DeviceProfile kProfiles[] = { };
const size_t kProfileCount = sizeof(kProfiles) / sizeof(kProfiles[0]);

const uint16 kVendor = 0x054C;
const uint16 kProduct = 0x0279;

// Vendor request bytes and the recipient/direction masks.
const uint8 kVendorOut = 0x40;
const uint8 kFirmwareLoad = 0xA0;			// EZ-USB bootloader firmware download
const uint16 kCpuCsRegister = 0xE600;
const uint8 kVendorIn = 0xC0;
const uint8 kRegisterRead = 0x21;
const uint8 kRegisterWrite = 0x20;
const uint8 kSetMode = 0x23;
const uint8 kSerialWrite = 0x2C;		// 3-wire bus on port 0: 8-bit address,
const uint8 kSerialRead = 0x2B;		// 16-bit data, LSB first on the wire
const uint8 kFifoReset = 0x27;

// I2C devices behind the bridge (7-bit addresses).
const uint8 kDemod = 0x6E;
const uint8 kPage2 = 0x6C;

// Everything below is the sequence Sony's own DtvCore.dll sends for this
// module - "tuner type 3", which DtvCore picks when EEPROM byte 0x18 reads 2 -
// recovered by running that code under an x86 emulator and logging every
// vendor request it issued (AGENTS.md §6). Values are (register, value) pairs.
//
// Bus layout: 0x6E is the demodulator, 0x6C its second register page, and the
// RF tuner sits behind the demodulator's I2C repeater at 0x63, reachable only
// while demodulator register 0x42 holds 0x10.

// Demodulator, before its DSP program is loaded.
const uint8 kDemodPreload[][2] = {
	{ 0x42, 0x01 }, { 0x42, 0x00 }, { 0x41, 0x01 }, { 0x42, 0x04 },
	{ 0xf9, 0x1c },
};
// After the program: start it, pulse the core, then a few patch words.
const uint8 kDemodStart[][2] = {
	{ 0xf9, 0x11 }, { 0xf9, 0x10 }, { 0x42, 0x00 },
};
const uint8 kDemodPatch[][2] = {
	{ 0x42, 0x04 }, { 0xf9, 0x90 }, { 0x6e, 0x6f }, { 0x6f, 0x00 },
	{ 0x75, 0x55 }, { 0x76, 0x7d }, { 0x77, 0x00 }, { 0xf9, 0x10 },
	{ 0x42, 0x00 }, { 0x54, 0x10 },
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
// The demodulator's second page, once at bring-up.
const uint8 kPage6C[][2] = {
	{ 0x00, 0x20 }, { 0x92, 0x00 }, { 0x98, 0x80 }, { 0x99, 0x02 },
	{ 0xdf, 0x01 }, { 0xf5, 0x10 },
};

// The RF tuner takes one I2C write of (register, value) pairs ended by 0xFE.
// Its bring-up, which also tunes UHF 13:
const uint8 kTunerInit[] = {
	0x0a, 0xfd, 0x0c, 0x49, 0x0d, 0x86, 0x0f, 0xf0, 0x14, 0xc0, 0x15, 0xc0,
	0x1a, 0xd6, 0x1b, 0x1c, 0x1c, 0xca, 0x1d, 0x01, 0x1e, 0xfc, 0x24, 0x40,
	0x28, 0xf9, 0x29, 0x33, 0x32, 0x02, 0x3c, 0x44, 0x45, 0x3f, 0x6b, 0x00,
	0x66, 0x0f, 0x0e, 0x00, 0x33, 0x76, 0x34, 0x44, 0x35, 0x92, 0x36, 0x49,
	0x09, 0xf0, 0xfe,
};
// Per channel. Bytes 13 and 15 are the PLL word, 0x7644 for UHF 13 and
// 0x180 more per 6 MHz channel - 1/64 MHz steps.
const uint8 kTunerChannel[] = {
	0x0d, 0x86, 0x1a, 0xd6, 0x1e, 0xfc, 0x24, 0x40, 0x6b, 0x00, 0x0e, 0x00,
	0x33, 0x00, 0x34, 0x00, 0x35, 0x92, 0x36, 0x49, 0xfe,
};
const size_t kTunerChannelHigh = 13;
const size_t kTunerChannelLow = 15;
// Around the channel write: hold the tuner, then release it.
const uint8 kTunerHold[] = { 0x09, 0xf0, 0xfe };
const uint8 kTunerRun[] = { 0x09, 0xf4, 0xfe };

// wIndexH 0x7F reaches the firmware's own settings block instead of the bus:
// 0x08 selects the antenna input, 0x09 a second switch, and writing 1 to 0x0B
// pulses the reset line of the chip on the 3-wire bus.
const uint8 kFirmwareSettings = 0x7F;

const uint8 kTunerAddress = 0x63;
const uint8 kRepeaterOpen = 0x10;		// demodulator 0x42

// The demodulator's DSP program: 988 bytes, written 16 at a time through a
// window - 0x6E/0x6F take the address (0x5000 up), 0x70 the data. It is
// Sony's code and is not in this repository; recovery/extract_demod.py pulls
// it out of the owner's own DtvCore.dll.
const size_t kDemodProgramSize = 988;
const uint16 kDemodProgramBase = 0x5000;


// Where the frequency word might land, best-supported first. The first two are
// the two readings of DtvCore.dll (see AGENTS.md §6.4); the rest are their near
// neighbours - the registers the demodulator init table deliberately leaves at
// zero, paired both ways, with each latch value that appears in the vendor
// code. Short on purpose: this runs against one channel at a time, and a list
// long enough to be "thorough" would be too slow to sit through.
const UsbTuner::TuningCandidate kTuningCandidates[] = {
	{ 0x64, 0x67, 0x10 },		// the default: DtvCore.dll 0x100905f7, and the
								// only layout the chip takes in (diagnostic)
	{ 0x32, 0x33, 0x01 },		// the former default
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


// Where the firmware image and the demodulator program are looked for, in
// order. Shared by the loaders and the USB report, so the report cannot claim
// a file the loader would not find - the whole point of printing it.
std::string
FindDataFile(const char* name, const std::string& preferred)
{
	const char* directories[] = {
		"/boot/home/config/settings/roneseg/",
		"/boot/home/config/non-packaged/data/roneseg/",
		"/boot/home/fwtool/",
		"",
	};
	std::vector<std::string> candidates;
	if (!preferred.empty())
		candidates.push_back(preferred);
	for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++)
		candidates.push_back(std::string(directories[i]) + name);

	for (size_t i = 0; i < candidates.size(); i++) {
		FILE* file = fopen(candidates[i].c_str(), "rb");
		if (file != NULL) {
			fclose(file);
			return candidates[i];
		}
	}
	return std::string();
}


std::string
FindFirmwareFile(const std::string& preferred)
{
	return FindDataFile("oneseg_fw.rec", preferred);
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
	// 0x64/0x67 with latch 0x10: the only layout the demodulator was seen
	// to take in (register 0x10 then holds V's high byte on every channel -
	// see the tuner diagnostic in AGENTS.md). Until 2026-09 this was
	// 0x32/0x33 latch 0x01, which the chip ignored.
	fFrequencyReg(0x64),
	fFrequencyRegLow(0x67),
	fLatchValue(0x10)
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
	// Whether the firmware image is where the loader will look for it. Printed
	// unconditionally, because its absence is invisible otherwise: the module
	// enumerates, answers control transfers, and still cannot stream, so a
	// scan finds nothing and the hardware looks broken when it is not.
	std::string firmware;
	std::string path = FindFirmwareFile(std::string());
	if (path.empty()) {
		firmware =
			"Firmware: oneseg_fw.rec NOT FOUND.\n"
			"  The module powers up blank. Until its firmware is uploaded it\n"
			"  exposes no streaming endpoint, so a scan finds nothing however\n"
			"  healthy the hardware is - which is what an absent image looks\n"
			"  like from the outside.\n"
			"  Looked in ~/config/settings/roneseg/,\n"
			"  ~/config/non-packaged/data/roneseg/, ~/fwtool/ and the current\n"
			"  directory. Extract it from your own machine's Windows driver\n"
			"  with recovery/extract_fw.py - see FIRMWARE.md.\n";
	} else {
		firmware = "Firmware: " + path + "\n";
	}

	std::vector<Candidate> candidates = Scan();

	std::string report;
	if (candidates.empty()) {
		report =
			"No USB device on this machine looks like a tuner.\n"
			"\n"
			"On a VAIO P the internal module sits on a UHCI companion, which\n"
			"enumerates nothing without the SCH USBLEGSUP fix from the VAIO P\n"
			"patch set - check you are running that ISO - and the module may\n"
			"only appear a few seconds after boot.\n"
			"\n" + firmware;
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
		"firmware on the first tune, after which it renumerates as \"CXD9192\n"
		"Controller\" and streams. That upload needs the image below.\n"
		"\n" + firmware;
	return report;
}


// #pragma mark - the hardware path


std::string
UsbTuner::LocateFirmware() const
{
	return FindFirmwareFile(fFirmwarePath);
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
		SetLastError("the module needs its firmware: put oneseg_fw.rec in "
			"~/config/settings/roneseg/ (extract it with "
			"recovery/extract_fw.py - see FIRMWARE.md)");
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


bool
UsbTuner::ReadRegisters(uint8 sub, uint8 base, uint8* out, uint8 count)
{
	if (out == NULL || count == 0 || count > 64)
		return false;
	return ControlTimed(kVendorIn, kRegisterRead, count,
		(uint16)((sub << 8) | base), count, out, 1000000) == (ssize_t)count;
}


bool
UsbTuner::WriteBlock(uint8 sub, uint8 reg, const uint8* data, uint8 count)
{
	// wValue = byte count; the data stage carries them, and the firmware puts
	// the register and all of them on the bus as one I2C write.
	return ControlTimed(kVendorOut, kRegisterWrite, count,
		(uint16)((sub << 8) | reg), count, (void*)data, 1000000)
		== (ssize_t)count;
}


bool
UsbTuner::WriteTable(uint8 sub, const uint8 (*table)[2], size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (!WriteRegister(sub, table[i][0], table[i][1]))
			return false;
	}
	return true;
}


// The tuner stream is (register, value) pairs, so its first byte is the
// register the I2C write starts at and the rest goes as data.
bool
UsbTuner::WriteTuner(const uint8* stream, size_t size)
{
	return WriteBlock(kTunerAddress, stream[0], stream + 1, (uint8)(size - 1));
}


status_t
UsbTuner::LoadDemodProgram(std::vector<uint8>* program)
{
	std::string path = FindDataFile("oneseg_demod.bin", std::string());
	if (path.empty()) {
		SetLastError("the demodulator needs its program: put oneseg_demod.bin "
			"in ~/config/settings/roneseg/ (extract it from DtvCore.dll with "
			"recovery/extract_demod.py - see FIRMWARE.md)");
		return B_ENTRY_NOT_FOUND;
	}
	FILE* file = fopen(path.c_str(), "rb");
	if (file == NULL)
		return B_IO_ERROR;
	program->resize(kDemodProgramSize + 1);
	size_t got = fread(&(*program)[0], 1, program->size(), file);
	fclose(file);
	if (got != kDemodProgramSize) {
		SetLastError("oneseg_demod.bin is not the 988-byte demodulator program");
		return B_BAD_DATA;
	}
	program->resize(kDemodProgramSize);
	return B_OK;
}


bool
UsbTuner::SerialRead(uint8 reg, uint16* value)
{
	uint8 data[2];
	if (ControlTimed(kVendorIn, kSerialRead, 0, reg, 2, data, 1000000) != 2)
		return false;
	*value = data[0] | (data[1] << 8);
	return true;
}


bool
UsbTuner::SerialWrite(uint8 reg, uint16 value)
{
	uint8 data[2] = { (uint8)(value & 0xff), (uint8)(value >> 8) };
	return ControlTimed(kVendorOut, kSerialWrite, 2, reg, 2, data, 1000000)
		== 2;
}


// The chip on the 3-wire bus sits in the transport-stream path (DtvCore
// calls it "Leira"). Reset it, wait for its ready bit, and switch the stream
// through - DtvCore's 0x10091c00.
status_t
UsbTuner::StartStreamPath()
{
	if (!WriteRegister(kFirmwareSettings, 0x0b, 0x01))
		return B_IO_ERROR;
	uint16 value = 0;
	bigtime_t deadline = system_time() + 1000000;
	while (!SerialRead(0x2c, &value) || (value & 0x0002) == 0) {
		if (system_time() >= deadline) {
			SetLastError("the stream path chip did not come out of reset");
			return B_TIMED_OUT;
		}
		snooze(1000);
	}
	uint16 control = 0, enable = 0;
	if (!SerialWrite(0x1a, 0x0000) || !SerialWrite(0x1a, 0x8000)
		|| !SerialRead(0x22, &control) || !SerialWrite(0x22, control | 0x0100)
		|| !SerialRead(0x24, &enable) || !SerialWrite(0x24, enable | 0x0001))
		return B_IO_ERROR;
	return B_OK;
}


status_t
UsbTuner::BringUp()
{
	std::vector<uint8> program;
	status_t status = LoadDemodProgram(&program);
	if (status != B_OK)
		return status;

	if (ControlTimed(kVendorOut, kSetMode, 1, 0, 0, NULL, 1000000) < 0)
		return B_DEVICE_NOT_FOUND;

	status = StartStreamPath();
	if (status != B_OK)
		return status;
	if (!WriteRegister(kFirmwareSettings, 0x09, 0x00)
		|| !WriteRegister(kFirmwareSettings, 0x08, 0x00))
		return B_IO_ERROR;

	if (!WriteTable(kDemod, kDemodPreload,
			sizeof(kDemodPreload) / sizeof(kDemodPreload[0])))
		return B_IO_ERROR;
	for (size_t offset = 0; offset < program.size(); offset += 16) {
		uint16 address = kDemodProgramBase + offset;
		size_t chunk = program.size() - offset < 16
			? program.size() - offset : 16;
		if (!WriteRegister(kDemod, 0x6e, address >> 8)
			|| !WriteRegister(kDemod, 0x6f, address & 0xff)
			|| !WriteBlock(kDemod, 0x70, &program[offset], chunk))
			return B_IO_ERROR;
	}
	if (!WriteTable(kDemod, kDemodStart,
			sizeof(kDemodStart) / sizeof(kDemodStart[0])))
		return B_IO_ERROR;
	snooze(5000);
	if (!WriteTable(kDemod, kDemodPatch,
			sizeof(kDemodPatch) / sizeof(kDemodPatch[0]))
		|| !WriteTable(kDemod, kDemodInit,
			sizeof(kDemodInit) / sizeof(kDemodInit[0])))
		return B_IO_ERROR;

	if (!WriteRegister(kDemod, 0x42, kRepeaterOpen)
		|| !WriteTuner(kTunerInit, sizeof(kTunerInit))
		|| !WriteRegister(kDemod, 0x42, 0x00))
		return B_IO_ERROR;

	if (!WriteTable(kPage2, kPage6C, sizeof(kPage6C) / sizeof(kPage6C[0])))
		return B_IO_ERROR;
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

	// Japan's UHF raster: channel 13 at 473 + 1/7 MHz, 6 MHz apart.
	int channel = (int)lround(((double)frequencyHz / 1000000.0
		- (473.0 + 1.0 / 7.0)) / 6.0) + 13;
	if (channel < 13 || channel > 62) {
		SetLastError("not a Japanese UHF channel (13-62)");
		return B_BAD_VALUE;
	}

	uint8 stream[sizeof(kTunerChannel)];
	memcpy(stream, kTunerChannel, sizeof(stream));
	uint16 word = 0x7644 + 0x180 * (channel - 13);
	stream[kTunerChannelHigh] = word >> 8;
	stream[kTunerChannelLow] = word & 0xff;

	// Hold the demodulator (0x41 bit 0) while the tuner moves, open the
	// repeater, retune, close it, let the PLL settle, release.
	uint8 control = 0;
	if (!ReadRegisters(kDemod, 0x41, &control, 1)
		|| !WriteRegister(kDemod, 0x41, control | 0x01)
		|| !WriteRegister(kDemod, 0x42, kRepeaterOpen)
		|| !WriteTuner(kTunerHold, sizeof(kTunerHold))
		|| !WriteTuner(stream, sizeof(stream))
		|| !WriteTuner(kTunerRun, sizeof(kTunerRun))
		|| !WriteRegister(kDemod, 0x42, 0x00)) {
		SetLastError("could not program the tuner");
		return B_IO_ERROR;
	}
	snooze(100000);

	if (!WriteRegister(kDemod, 0x4b, 0x16)
		|| !ReadRegisters(kDemod, 0x41, &control, 1)
		|| !WriteRegister(kDemod, 0x41, control & ~0x01)
		|| !WriteRegister(kPage2, 0x04, 0x02)
		|| !WriteRegister(kPage2, 0xf2, 0x01)) {
		SetLastError("could not restart the demodulator");
		return B_IO_ERROR;
	}

	fFrequency = frequencyHz;
	return B_OK;
}


// DtvCore's own test: register 0x00 bit 1 together with 0x02 bit 3 is a
// lock; 0x00 bit 3 on its own is the demodulator giving up. Polled every
// 100 ms for up to 1.5 s, as DtvCore does.
UsbTuner::LockState
UsbTuner::WaitForLock(bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	while (true) {
		uint8 status[3];
		if (!ReadRegisters(kDemod, 0x00, status, 3))
			return kLockError;
		if ((status[0] & 0x02) != 0 && (status[2] & 0x08) != 0)
			return kLocked;
		if ((status[0] & 0x08) != 0)
			return kNoSignal;
		if (system_time() >= deadline)
			return kNoSignal;
		snooze(100000);
	}
}


ssize_t
UsbTuner::BulkRead(void* buffer, size_t size, bigtime_t timeout)
{
	const BUSBEndpoint* endpoint = fStreamEndpoint;
	if (endpoint == NULL)
		return B_NO_INIT;
	// A zero-length bulk transfer panics Haiku's EHCI driver (no descriptor
	// is built and FillQueueWithData dereferences it).
	if (size == 0)
		return 0;

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
	fDiagnostic.lock = WaitForLock(1500000);
	if (fDiagnostic.lock != kLocked)
		return false;

	const size_t kSize = 16384;
	uint8* buffer = (uint8*)malloc(kSize);
	if (buffer == NULL)
		return false;

	// Only read once the demodulator says it has locked: a bulk read with
	// nothing to deliver leaves later control requests stuck (AGENTS.md).
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
