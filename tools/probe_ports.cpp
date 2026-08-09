// Report the connect status of every USB port on the machine, read straight
// out of the host controllers' own registers.
//
// WHY THIS EXISTS
//
// The VAIO P carries its One-Seg tuner on an internal expansion slot that is
// wired as USB - the same slot a wireless WAN + GPS module goes into on the
// models that have that instead. If the module is fitted and nothing appears
// in listusb, there are two very different explanations, and the USB stack
// cannot tell them apart because in both cases it simply never sees a
// device:
//
//   - the port has a device on it that failed to enumerate, or
//   - the port has no device, or no power reaching one.
//
// The host controller knows. Every UHCI and EHCI port has a Current Connect
// Status bit that is set by the hardware the moment a device's pull-up
// resistor is detected, before any enumeration is attempted. Reading that
// bit for every port answers the question directly: a set bit on a port with
// nothing enumerated means the module is there and something later in the
// chain is broken; no set bit anywhere means the slot is empty or unpowered.
//
// Read-only. It reads PCI configuration space and the controllers' port
// status registers and writes nothing at all.
//
// Build:  setarch x86 g++ -o probe_ports probe_ports.cpp
// Run:    ./probe_ports

#include <Drivers.h>
#include <PCI.h>
#include <OS.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <private/drivers/poke.h>

static int sPoke = -1;


static status_t
PciRead(uint8 bus, uint8 device, uint8 function, uint8 offset, uint8 size,
	uint32* value)
{
	pci_io_args args;
	args.signature = POKE_SIGNATURE;
	args.bus = bus;
	args.device = device;
	args.function = function;
	args.size = size;
	args.offset = offset;
	args.value = 0;

	status_t status = ioctl(sPoke, POKE_PCI_READ_CONFIG, &args, sizeof(args));
	if (status == B_OK)
		*value = args.value;
	return status;
}


static status_t
PortRead(uint16 port, uint8 size, uint32* value)
{
	port_io_args args;
	args.signature = POKE_SIGNATURE;
	args.port = port;
	args.size = size;
	args.value = 0;

	status_t status = ioctl(sPoke, POKE_PORT_READ, &args, sizeof(args));
	if (status == B_OK)
		*value = args.value;
	return status;
}


// #pragma mark - UHCI


// PORTSC1 and PORTSC2 sit at base+0x10 and base+0x12, 16 bits each.
// Bit 0 is Current Connect Status, bit 2 is Port Enabled.
static void
DumpUhci(uint8 bus, uint8 device, uint8 function, uint16 deviceId)
{
	uint32 bar4 = 0;
	if (PciRead(bus, device, function, 0x20, 4, &bar4) != B_OK)
		return;
	uint16 base = (uint16)(bar4 & 0xFFE0);
	if (base == 0) {
		printf("  UHCI %04x at %u:%u:%u - no I/O base\n", deviceId, bus,
			device, function);
		return;
	}

	printf("  UHCI %04x at %u:%u:%u, I/O base 0x%04x\n", deviceId, bus,
		device, function, base);

	for (int port = 0; port < 2; port++) {
		uint32 status = 0;
		if (PortRead(base + 0x10 + port * 2, 2, &status) != B_OK) {
			printf("    port %d: read failed\n", port);
			continue;
		}
		printf("    port %d: 0x%04x  %s%s%s\n", port, status,
			(status & 0x0001) != 0 ? "DEVICE CONNECTED" : "empty",
			(status & 0x0004) != 0 ? ", enabled" : "",
			(status & 0x0200) != 0 ? ", suspended" : "");
	}
}


// #pragma mark - EHCI


static void
DumpEhci(uint8 bus, uint8 device, uint8 function, uint16 deviceId)
{
	uint32 bar0 = 0;
	if (PciRead(bus, device, function, 0x10, 4, &bar0) != B_OK)
		return;
	phys_addr_t physical = bar0 & ~0x0FUL;
	if (physical == 0) {
		printf("  EHCI %04x at %u:%u:%u - no MMIO base\n", deviceId, bus,
			device, function);
		return;
	}

	// The controller only answers on this BAR while memory decoding is
	// enabled in its command register. It normally is, but checking beats
	// reading back 0xffffffff and calling every port empty.
	uint32 command = 0;
	if (PciRead(bus, device, function, 0x04, 2, &command) == B_OK
		&& (command & 0x02) == 0) {
		printf("  EHCI %04x at %u:%u:%u - memory decoding disabled\n",
			deviceId, bus, device, function);
		return;
	}

	mem_map_args args;
	memset(&args, 0, sizeof(args));
	args.signature = POKE_SIGNATURE;
	args.name = "ehci-regs";
	args.physical_address = physical;
	args.size = B_PAGE_SIZE;
	args.flags = B_ANY_KERNEL_ADDRESS;
	args.protection = B_READ_AREA | B_WRITE_AREA;

	area_id cloned = -1;
	void* local = NULL;
	uint8 fallback[256];

	if (ioctl(sPoke, POKE_MAP_MEMORY, &args, sizeof(args)) == B_OK) {
		// poke maps the registers into the kernel and hands back the area,
		// not a pointer this process can follow. Cloning is what brings the
		// mapping into our address space.
		cloned = clone_area("ehci-regs-local", &local, B_ANY_ADDRESS,
			B_READ_AREA | B_WRITE_AREA, args.area);
		if (cloned < 0) {
			ioctl(sPoke, POKE_UNMAP_MEMORY, &args, sizeof(args));
			local = NULL;
		}
	}

	if (local == NULL) {
		// Fall back to reading the same physical addresses through
		// /dev/misc/mem, which is how tools/dump_acpi.c reads the ACPI
		// tables. Only the first 256 bytes are needed - capability
		// registers plus the port status array - and PORTSC is
		// read-to-observe, so a plain read is enough.
		int mem = open("/dev/misc/mem", O_RDONLY);
		if (mem < 0) {
			printf("  EHCI %04x at %u:%u:%u - no way to reach 0x%08lx\n",
				deviceId, bus, device, function, (unsigned long)physical);
			return;
		}
		if (lseek(mem, (off_t)physical, SEEK_SET) == (off_t)-1
			|| read(mem, fallback, sizeof(fallback)) != (ssize_t)sizeof(fallback)) {
			printf("  EHCI %04x at %u:%u:%u - could not read 0x%08lx: %s\n",
				deviceId, bus, device, function, (unsigned long)physical,
				strerror(errno));
			close(mem);
			return;
		}
		close(mem);
		local = fallback;
		printf("  (read through /dev/misc/mem)\n");
	}

	volatile uint8* registers = (volatile uint8*)local;
	uint8 capabilityLength = registers[0];
	uint32 structural = *(volatile uint32*)(registers + 0x04);
	int ports = structural & 0x0F;

	printf("  EHCI %04x at %u:%u:%u, MMIO 0x%08lx, %d port(s)\n", deviceId,
		bus, device, function, (unsigned long)physical, ports);

	volatile uint32* portsc
		= (volatile uint32*)(registers + capabilityLength + 0x44);
	for (int port = 0; port < ports; port++) {
		uint32 status = portsc[port];
		// Bit 13 is Port Owner: set means a companion controller has this
		// port, which is what happens to every full-speed device.
		printf("    port %d: 0x%08" B_PRIx32 "  %s%s%s\n", port, status,
			(status & 0x0001) != 0 ? "DEVICE CONNECTED" : "empty",
			(status & 0x0004) != 0 ? ", enabled" : "",
			(status & 0x2000) != 0 ? ", owned by companion" : "");
	}

	if (cloned >= 0) {
		delete_area(cloned);
		ioctl(sPoke, POKE_UNMAP_MEMORY, &args, sizeof(args));
	}
}


int
main()
{
	sPoke = open(POKE_DEVICE_FULLNAME, O_RDWR);
	if (sPoke < 0) {
		fprintf(stderr, "cannot open %s - is the poke driver present?\n",
			POKE_DEVICE_FULLNAME);
		return 1;
	}

	printf("USB host controllers and the connect status of every port:\n\n");

	pci_info info;
	pci_info_args args;
	args.signature = POKE_SIGNATURE;
	args.info = &info;

	int found = 0;
	for (uint8 index = 0; index < 255; index++) {
		args.index = index;
		args.status = B_ERROR;
		if (ioctl(sPoke, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) != B_OK)
			break;
		if (args.status != B_OK)
			break;

		if (info.class_base != 0x0C || info.class_sub != 0x03)
			continue;

		found++;
		// prog_if: 0x00 UHCI, 0x10 OHCI, 0x20 EHCI.
		if (info.class_api == 0x20)
			DumpEhci(info.bus, info.device, info.function, info.device_id);
		else
			DumpUhci(info.bus, info.device, info.function, info.device_id);
	}

	close(sPoke);

	if (found == 0) {
		printf("no USB controllers found\n");
		return 1;
	}

	printf(
		"\nRead this against listusb. A port reporting DEVICE CONNECTED with\n"
		"no corresponding entry in listusb means hardware is physically\n"
		"present on that port and something after the connect detection is\n"
		"failing. Every port reporting empty means there is nothing on them\n"
		"drawing enough power to assert its pull-up - an empty slot, or a\n"
		"module whose power rail is off.\n");
	return 0;
}
