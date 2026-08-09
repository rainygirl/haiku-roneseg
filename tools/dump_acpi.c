/*
 * Dump the machine's ACPI DSDT and SSDTs by reading physical memory through
 * /dev/misc/mem, so they can be disassembled with iasl elsewhere.
 *
 * Haiku ships no acpidump, and the question this is meant to answer needs the
 * real tables off this specific unit: is there an EC/SNC method that powers
 * the One-Seg tuner module, the way F124's sub-functions power WLAN (WLPW)
 * and Bluetooth (BTPW)? If such a method exists it is in here. If it does not,
 * the module has no software power control and that is the end of it.
 *
 * Read-only. It maps nothing and writes nothing.
 *
 * Build:  gcc -o dump_acpi dump_acpi.c
 * Run:    ./dump_acpi /boot/home/acpi
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

static int sMemFd = -1;


static int
ReadPhysical(uint64_t address, void *buffer, size_t size)
{
	if (lseek(sMemFd, (off_t)address, SEEK_SET) == (off_t)-1)
		return -1;

	size_t done = 0;
	while (done < size) {
		ssize_t got = read(sMemFd, (char *)buffer + done, size - done);
		if (got <= 0)
			return -1;
		done += got;
	}
	return 0;
}


struct AcpiHeader {
	char		signature[4];
	uint32_t	length;
	uint8_t		revision;
	uint8_t		checksum;
	char		oemId[6];
	char		oemTableId[8];
	uint32_t	oemRevision;
	char		creatorId[4];
	uint32_t	creatorRevision;
} __attribute__((packed));


static int
DumpTable(const char *directory, uint64_t address, int index)
{
	struct AcpiHeader header;
	if (ReadPhysical(address, &header, sizeof(header)) != 0)
		return -1;

	/* A plausible table has a printable signature and a sane length. */
	if (header.length < sizeof(header) || header.length > 1024 * 1024)
		return -1;

	char signature[5];
	memcpy(signature, header.signature, 4);
	signature[4] = '\0';

	unsigned char *table = malloc(header.length);
	if (table == NULL)
		return -1;
	if (ReadPhysical(address, table, header.length) != 0) {
		free(table);
		return -1;
	}

	char path[512];
	if (index < 0)
		snprintf(path, sizeof(path), "%s/%s.dat", directory, signature);
	else
		snprintf(path, sizeof(path), "%s/%s-%d.dat", directory, signature, index);

	FILE *out = fopen(path, "wb");
	if (out == NULL) {
		free(table);
		return -1;
	}
	fwrite(table, 1, header.length, out);
	fclose(out);

	printf("  %-6s %6u bytes at 0x%08llx  oem=%.6s/%.8s  -> %s\n",
		signature, header.length, (unsigned long long)address,
		header.oemId, header.oemTableId, path);

	free(table);
	return 0;
}


int
main(int argc, char **argv)
{
	const char *directory = argc > 1 ? argv[1] : ".";

	sMemFd = open("/dev/misc/mem", O_RDONLY);
	if (sMemFd < 0) {
		fprintf(stderr, "cannot open /dev/misc/mem\n");
		return 1;
	}

	/* The RSDP lives on a 16-byte boundary in the BIOS area. The EBDA is
	   also legal per spec, but on this class of firmware the BIOS area
	   always has it, and scanning that is enough. */
	uint64_t rsdpAddress = 0;
	unsigned char window[16];
	for (uint64_t address = 0xe0000; address < 0x100000; address += 16) {
		if (ReadPhysical(address, window, sizeof(window)) != 0)
			continue;
		if (memcmp(window, "RSD PTR ", 8) == 0) {
			rsdpAddress = address;
			break;
		}
	}

	if (rsdpAddress == 0) {
		fprintf(stderr, "no RSDP found in 0xe0000-0xfffff\n");
		return 1;
	}

	unsigned char rsdp[36];
	if (ReadPhysical(rsdpAddress, rsdp, sizeof(rsdp)) != 0) {
		fprintf(stderr, "could not read the RSDP\n");
		return 1;
	}

	uint8_t revision = rsdp[15];
	uint32_t rsdtAddress = *(uint32_t *)(rsdp + 16);
	uint64_t xsdtAddress = revision >= 2 ? *(uint64_t *)(rsdp + 24) : 0;

	printf("RSDP at 0x%08llx, revision %u, RSDT 0x%08x, XSDT 0x%016llx\n",
		(unsigned long long)rsdpAddress, revision, rsdtAddress,
		(unsigned long long)xsdtAddress);

	/* Prefer the 32-bit RSDT: this is a 2009 Atom netbook, its XSDT is
	   frequently either absent or a duplicate, and every pointer fits in
	   32 bits anyway. */
	uint64_t rootAddress = rsdtAddress != 0 ? rsdtAddress : xsdtAddress;
	int entryWidth = rsdtAddress != 0 ? 4 : 8;
	if (rootAddress == 0) {
		fprintf(stderr, "neither RSDT nor XSDT is present\n");
		return 1;
	}

	struct AcpiHeader root;
	if (ReadPhysical(rootAddress, &root, sizeof(root)) != 0) {
		fprintf(stderr, "could not read the root table\n");
		return 1;
	}

	int count = (root.length - sizeof(root)) / entryWidth;
	printf("root table %.4s holds %d entries\n\n", root.signature, count);

	unsigned char *entries = malloc(count * entryWidth);
	if (entries == NULL)
		return 1;
	if (ReadPhysical(rootAddress + sizeof(root), entries,
			count * entryWidth) != 0) {
		fprintf(stderr, "could not read the root table entries\n");
		return 1;
	}

	int ssdtIndex = 0;
	uint64_t dsdtAddress = 0;

	for (int i = 0; i < count; i++) {
		uint64_t address = entryWidth == 4
			? *(uint32_t *)(entries + i * 4)
			: *(uint64_t *)(entries + i * 8);
		if (address == 0)
			continue;

		struct AcpiHeader header;
		if (ReadPhysical(address, &header, sizeof(header)) != 0)
			continue;

		if (memcmp(header.signature, "SSDT", 4) == 0)
			DumpTable(directory, address, ssdtIndex++);
		else
			DumpTable(directory, address, -1);

		/* The FADT carries the DSDT's address; the DSDT itself is never
		   listed in the RSDT. */
		if (memcmp(header.signature, "FACP", 4) == 0) {
			uint32_t dsdt32 = 0;
			uint64_t dsdt64 = 0;
			ReadPhysical(address + 40, &dsdt32, sizeof(dsdt32));
			if (header.length >= 148)
				ReadPhysical(address + 140, &dsdt64, sizeof(dsdt64));
			dsdtAddress = dsdt32 != 0 ? dsdt32 : dsdt64;
		}
	}

	if (dsdtAddress != 0) {
		printf("\nDSDT (from the FADT):\n");
		DumpTable(directory, dsdtAddress, -1);
	} else {
		fprintf(stderr, "\nno DSDT address in the FADT\n");
	}

	free(entries);
	close(sMemFd);

	printf("\nDisassemble elsewhere with:  iasl -d *.dat\n");
	return 0;
}
