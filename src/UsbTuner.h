#ifndef RONESEG_USB_TUNER_H
#define RONESEG_USB_TUNER_H

#include <USBKit.h>
#include <Locker.h>

#include "Tuner.h"

#include <string>
#include <vector>

// The real front end, driven from userland through the USB Kit.
//
// No kernel driver. BUSBDevice/BUSBEndpoint expose the control and bulk
// transfers a demodulator module needs: control transfers to configure and
// tune it, one bulk IN endpoint to pull the transport stream off it.
//
// One caveat specific to this machine: the internal module hangs off a UHCI
// companion controller, and on the SCH chipset those do not enumerate anything
// without the USBLEGSUP fix in the VAIO P patch set. On an unpatched Haiku this
// class simply finds no device.
//
// The module (054c:0279) is a Cypress EZ-USB that powers up blank: it answers
// control transfers, reports bcdDevice 0, and streams on no endpoint until an
// 8051 firmware image is uploaded. Open() uploads it automatically (from a
// oneseg_fw.rec file, see LocateFirmware()), after which the device renumerates
// as "CXD9192 Controller" and streams. The upload is to RAM, so a power cycle
// returns it to the blank state.
//
// Open() then runs the fixed bring-up (demodulator + RF front end), Tune()
// programs the frequency, and Read() pulls the transport stream. The scanner
// (HasSignal) tests a channel by whether TS packets actually arrive, so it does
// not depend on identifying the demodulator's lock register.
class UsbTuner : public Tuner {
public:
	// One entry per supported chip. The internal module is opened by identity
	// in the .cpp; this stays as the extension point for external USB tuners.
	struct DeviceProfile {
		uint16		vendorId;
		uint16		productId;
		const char*	name;

		// Some modules enumerate in a bootloader identity, take a firmware
		// blob, then re-enumerate under a different product ID. When this is
		// set and the file is absent, Open() says so plainly instead of
		// timing out on a device that is never going to answer.
		const char*	firmwareFile;

		// Filled in per chip. Each returns B_OK or a status_t.
		status_t	(*Init)(BUSBDevice& device);
		status_t	(*Tune)(BUSBDevice& device, uint64 frequencyHz);
		status_t	(*ReadStatus)(BUSBDevice& device, Status* out);
	};

	// A device that looks like it could be a tuner, for the report the app
	// prints when nothing matches a profile.
	struct Candidate {
		uint16		vendorId;
		uint16		productId;
		std::string	manufacturer;
		std::string	product;
		std::string	detail;		// descriptor summary, several lines
		bool		matchesProfile;
	};

	UsbTuner();
	virtual ~UsbTuner();

	virtual status_t Open();
	virtual void Close();
	virtual status_t Tune(uint64 frequencyHz);
	virtual status_t GetStatus(Status* out);
	virtual ssize_t Read(void* buffer, size_t size);
	virtual std::string Description() const;

	// Where to find the 8051 firmware image uploaded to a blank module. If
	// unset, a few standard locations are tried. See LocateFirmware().
	void SetFirmwarePath(const std::string& path) { fFirmwarePath = path; }

	// Tune the given channel and look at the data endpoint just long enough to
	// say whether a transport stream is coming out of it. This is the signal
	// test the scanner uses - it does not depend on knowing which demodulator
	// register is the lock bit, only on whether TS packets actually arrive.
	bool HasSignal(uint64 frequencyHz, bigtime_t timeout = 500000);

	// What the last HasSignal()/Read attempt saw, for the diagnostic log: how
	// many bytes came off the data endpoint and whether they framed as TS.
	struct Diagnostic {
		ssize_t	bytes;		// bytes read, 0 = nothing arrived, <0 = error
		bool	sync;		// TS sync bytes found
		bool	tuned;		// the tune write itself succeeded
		Diagnostic() : bytes(0), sync(false), tuned(false) {}
	};
	Diagnostic LastDiagnostic() const { return fDiagnostic; }

	// Which demodulator register the frequency word is written to. The value
	// recovered from static analysis is 0x32/0x33, but it could not be
	// confirmed against a live signal, so it is adjustable. reg and reg+1 take
	// the high and low bytes of V = 7 x f_MHz.
	void SetFrequencyRegister(uint8 reg) { fFrequencyReg = reg; }
	uint8 FrequencyRegister() const { return fFrequencyReg; }

	// Enumerates every USB device on the machine and describes the ones that
	// could plausibly be a tuner. Safe to call without Open(); this is the
	// first thing to run on a machine whose tuner has never been identified.
	static std::vector<Candidate> Scan();
	static std::string ScanReport();

	// Public because the BUSBRoster subclass that walks the bus lives in an
	// anonymous namespace in the .cpp and is not a member of this class.
	static bool LooksLikeTuner(const BUSBDevice& device);

private:
	static const DeviceProfile* ProfileFor(uint16 vendor, uint16 product);

	// The internal module is a Cypress EZ-USB: 054c:0279 in both its blank
	// bootloader identity and, once firmware is running, its CXD9192 identity.
	status_t				FindDevice(bigtime_t timeout);
	status_t				UploadFirmware();
	status_t				BringUp();
	status_t				ClaimEndpoints();
	std::string				LocateFirmware() const;
	bool					WriteRegister(uint8 sub, uint8 reg, uint8 value);
	// A control transfer with a deadline. BUSBDevice::ControlTransfer has none
	// of its own, so a wedged device would hang the caller forever - which is
	// exactly what froze a scan and stopped the app from quitting. Everything
	// on the tuning path goes through this instead.
	ssize_t					ControlTimed(uint8 requestType, uint8 request,
								uint16 value, uint16 index, uint16 length,
								void* buffer, bigtime_t timeout);
	ssize_t					BulkRead(void* buffer, size_t size,
								bigtime_t timeout);

	BUSBRoster*				fRoster;
	BUSBDevice*				fDevice;
	BLocker					fDeviceLock;	// fDevice is touched by the roster thread
	const BUSBInterface*	fInterface;
	const BUSBEndpoint*		fStreamEndpoint;	// large bulk IN - the TS
	const BUSBEndpoint*		fStatusEndpoint;	// small bulk IN
	const DeviceProfile*	fProfile;
	std::string				fDescription;
	std::string				fFirmwarePath;
	bool					fReady;
	uint64					fFrequency;
	uint8					fFrequencyReg;
	Diagnostic				fDiagnostic;
};

#endif
