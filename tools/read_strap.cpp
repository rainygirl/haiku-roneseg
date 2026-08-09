// Read the board configuration strap the DSDT calls GPID.
//
// The VAIO P's internal expansion slot holds either the One-Seg tuner or a
// wireless WAN + GPS module, and the firmware has to know which. The DSDT
// says how it knows: two GPIO bits.
//
//     OperationRegion (GPIO, SystemIO, 0x0500, 0x40)
//     Field (GPIO, ByteAcc, NoLock, Preserve) { Offset (0x28), PID0, 1, PID1, 1 }
//
//     Method (GPID) { Return (PID0 | (PID1 << 1)) }
//
// So GPID is bits 0 and 1 of I/O port 0x528. SNC function F11D looks that
// value up in a four-entry table, DCID, which maps every strap to 0 except
// GPID == 1, which maps to 1 - so the firmware treats one specific board
// configuration as special.
//
// What the mapping means is not documented anywhere I could find, so this
// tool reports the raw value rather than interpreting it. What makes it worth
// reading at all is that it is a *hardware* strap: unlike anything in ACPI or
// the EC, it is set by how the board was built and populated, so it says
// something about this unit that software state cannot.
//
// Read-only, and only reads: GPIO status ports have no read side effects.
// Nothing is written.
//
// Build:  setarch x86 g++ -o read_strap read_strap.cpp
// Run:    ./read_strap

#include <Drivers.h>
#include <PCI.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <private/drivers/poke.h>

static int sPoke = -1;

static const uint16 kGpioBase = 0x0500;
static const uint16 kGpioSize = 0x40;
static const uint16 kStrapPort = 0x0528;


static bool
ReadPort(uint16 port, uint8* _value)
{
	port_io_args args;
	args.signature = POKE_SIGNATURE;
	args.port = port;
	args.size = 1;
	args.value = 0;

	if (ioctl(sPoke, POKE_PORT_READ, &args, sizeof(args)) != B_OK)
		return false;

	*_value = (uint8)args.value;
	return true;
}


int
main()
{
	sPoke = open(POKE_DEVICE_FULLNAME, O_RDWR);
	if (sPoke < 0) {
		fprintf(stderr, "cannot open %s\n", POKE_DEVICE_FULLNAME);
		return 1;
	}

	printf("GPIO block at 0x%04x, %u bytes:\n", kGpioBase, kGpioSize);
	for (uint16 offset = 0; offset < kGpioSize; offset++) {
		uint8 value = 0;
		if (offset % 16 == 0)
			printf("  %04x:", kGpioBase + offset);
		if (ReadPort(kGpioBase + offset, &value))
			printf(" %02x", value);
		else
			printf(" --");
		if (offset % 16 == 15)
			printf("\n");
	}

	uint8 strapByte = 0;
	if (!ReadPort(kStrapPort, &strapByte)) {
		fprintf(stderr, "\ncould not read the strap port 0x%04x\n", kStrapPort);
		close(sPoke);
		return 1;
	}

	int strap = strapByte & 0x03;
	printf("\nport 0x%04x = 0x%02x, so GPID = %d (PID0=%d, PID1=%d)\n",
		kStrapPort, strapByte, strap, strap & 1, (strap >> 1) & 1);

	// The one thing the DSDT does tell us about these values.
	printf("F11D maps this strap to %d via the DCID table%s\n",
		strap == 1 ? 1 : 0,
		strap == 1 ? " - the one value it treats as special" : "");

	close(sPoke);
	return 0;
}
