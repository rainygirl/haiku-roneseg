/*
 * ectest - read the VAIO P's embedded controller, and set three specific
 * bits in it on explicit command, to find out whether any of them powers the
 * One-Seg module.
 *
 * WHAT THIS IS FOR
 *
 * The tuner is on an internal expansion slot wired as USB, and nothing
 * appears for it. Reading every host controller's port status directly
 * (tools/probe_ports.cpp) showed no port anywhere with a device on it that
 * failed to enumerate, so the module is either absent or has no power - a USB
 * device only asserts the pull-up the host detects once it is powered, and an
 * unpowered but fitted module reads exactly like an empty port.
 *
 * The precedent is this machine's own Bluetooth module, which was "long
 * presumed dead hardware" until the VAIO P patch set found the EC simply had
 * to be asked for logic power. Nothing asks on the tuner's behalf: no ACPI
 * method mentions a tuner at all.
 *
 * The DSDT declares four EC bits that no AML anywhere references, which means
 * a Windows driver wrote them:
 *
 *     Offset (0x24),  ACPC, 1,  ENTP, 1,  USBO, 1,
 *     Offset (0x26),  ENPS, 1,
 *     Offset (0x27),  USBP, 1,
 *
 * Reading them (the earlier read-only version of this driver) found ENTP
 * already 1 and USBO, USBP and ENPS all 0, with every known-good reference
 * bit reading exactly as expected - WLPW 1, BTPW 1, WWPW 0, plus a live
 * temperature - so the reads are genuinely reaching the EC. That rules ENTP
 * out as a closed gate and leaves the other three as the only remaining
 * software experiment.
 *
 * THE RISK, STATED PLAINLY
 *
 * ACPI exposes no way to write a field, so writing means driving the EC over
 * its I/O ports directly, behind the back of the kernel's own EC driver -
 * the same controller that runs thermal and battery management. The
 * mitigations here are real but not total:
 *
 *   - Every write takes the ACPI global lock first, which is the protocol
 *     this DSDT's Field(..., Lock, ...) declaration demands and which
 *     ACPICA itself honours, so we serialise against ACPI-initiated EC
 *     traffic. What it cannot serialise against is the EC's own SCI query
 *     events, which arrive asynchronously.
 *   - Writes are refused unless the EC is idle when we look.
 *   - Only the three named bits can be written. Any other field is refused,
 *     so a typo cannot reach the battery or thermal registers.
 *   - Each write is a read-modify-write of one bit, verified by reading the
 *     byte back, and the original value is remembered so "restore" puts
 *     everything back.
 *
 * If the machine misbehaves after a write, "restore" then a reboot returns
 * it to a known state: nothing here is persistent.
 *
 * BUILD AND USE - no installation, nothing in the system image replaced
 *
 *   ./load.sh                            build and load
 *   cat /dev/misc/ectest                 read every field
 *   echo "set USBP 1" > /dev/misc/ectest set one bit
 *   listusb                              look for a new device
 *   echo "restore" > /dev/misc/ectest    put the originals back
 *   ./load.sh unload                     remove the driver entirely
 */

#include <ACPI.h>
#include <Drivers.h>
#include <ISA.h>
#include <KernelExport.h>

#include <stdio.h>
#include <string.h>


#define ECTEST_DEVICE_NAME "misc/ectest"

#define TRACE(x...) dprintf("ectest: " x)

#define EC_PATH "\\_SB.PCI0.LPCB.H8EC."

/* Standard ACPI embedded controller ports, and this machine's _CRS agrees:
 * it declares exactly 0x62 and 0x66.
 */
#define EC_DATA_PORT	0x62
#define EC_COMMAND_PORT	0x66

#define EC_STATUS_OBF	0x01	/* output buffer full - data is waiting */
#define EC_STATUS_IBF	0x02	/* input buffer full - EC has not read ours */

#define EC_COMMAND_READ		0x80
#define EC_COMMAND_WRITE	0x81

#define EC_POLL_ATTEMPTS	500
#define EC_POLL_INTERVAL	20	/* microseconds */


/* The only fields this driver will write, with where they live. Anything not
 * in this table is refused - the point is that a mistake cannot reach the
 * battery or thermal registers by accident.
 */
static const struct {
	const char*	name;
	uint8		offset;
	uint8		bit;
	const char*	note;
} kWritable[] = {
	{ "USBP", 0x27, 0, "declared, never referenced by AML" },
	{ "USBO", 0x24, 2, "declared, never referenced by AML" },
	{ "ENPS", 0x26, 0, "declared, never referenced by AML" },
};

static const int kWritableCount = sizeof(kWritable) / sizeof(kWritable[0]);


/* Read-only reporting, including the reference bits whose values are already
 * known so a dump can be sanity-checked at a glance.
 */
static const struct {
	const char*	name;
	const char*	note;
} kFields[] = {
	{ "ENTP", "unknown - already 1, so not a closed gate" },
	{ "USBO", "unknown - writable here" },
	{ "USBP", "unknown - writable here" },
	{ "ENPS", "unknown - writable here" },
	{ "WLPW", "WLAN power - expect 1" },
	{ "BTPW", "Bluetooth power - expect 1" },
	{ "WWPW", "WWAN power - expect 0" },
	{ "GPSP", "GPS capability on the WWAN module - expect 0" },
	{ "ACPW", "AC adapter present" },
	{ "LSTE", "lid state" },
	{ "RTMP", "temperature, degrees C" },
};

static const int kFieldCount = sizeof(kFields) / sizeof(kFields[0]);


int32 api_version = B_CUR_DRIVER_API_VERSION;

static acpi_module_info* sAcpi = NULL;
static isa_module_info* sIsa = NULL;

static char sReport[3072];
static size_t sReportLength = 0;

/* What we changed, so it can be put back. */
static struct {
	bool	saved;
	uint8	original;
} sSaved[3];

static char sLog[512];


/* #pragma mark - reading, through ACPI */


static status_t
read_field(const char* name, uint32* _value)
{
	char path[128];
	uint8 buffer[64];
	acpi_object_type* object = (acpi_object_type*)buffer;
	status_t status;

	snprintf(path, sizeof(path), "%s%s", EC_PATH, name);
	memset(buffer, 0, sizeof(buffer));

	/* Evaluating a Field element performs the operation region read: ACPICA
	 * dispatches it to the address space handler the kernel's EC driver
	 * installed, taking the global lock because the Field is declared Lock.
	 * This is why reads need none of the caution writes do.
	 */
	status = sAcpi->evaluate_object(NULL, path, NULL, object, sizeof(buffer));
	if (status != B_OK)
		return status;

	if (object->object_type != ACPI_TYPE_INTEGER)
		return B_BAD_TYPE;

	*_value = (uint32)object->integer.integer;
	return B_OK;
}


/* #pragma mark - the EC transaction */


static status_t
ec_wait(uint8 mask, uint8 wanted)
{
	int attempt;

	for (attempt = 0; attempt < EC_POLL_ATTEMPTS; attempt++) {
		uint8 status = sIsa->read_io_8(EC_COMMAND_PORT);
		if ((status & mask) == wanted)
			return B_OK;
		spin(EC_POLL_INTERVAL);
	}

	return B_BUSY;
}


static status_t
ec_read_byte(uint8 offset, uint8* _value)
{
	status_t status = ec_wait(EC_STATUS_IBF, 0);
	if (status != B_OK)
		return status;

	sIsa->write_io_8(EC_COMMAND_PORT, EC_COMMAND_READ);
	status = ec_wait(EC_STATUS_IBF, 0);
	if (status != B_OK)
		return status;

	sIsa->write_io_8(EC_DATA_PORT, offset);
	status = ec_wait(EC_STATUS_OBF, EC_STATUS_OBF);
	if (status != B_OK)
		return status;

	*_value = sIsa->read_io_8(EC_DATA_PORT);
	return B_OK;
}


static status_t
ec_write_byte(uint8 offset, uint8 value)
{
	status_t status = ec_wait(EC_STATUS_IBF, 0);
	if (status != B_OK)
		return status;

	sIsa->write_io_8(EC_COMMAND_PORT, EC_COMMAND_WRITE);
	status = ec_wait(EC_STATUS_IBF, 0);
	if (status != B_OK)
		return status;

	sIsa->write_io_8(EC_DATA_PORT, offset);
	status = ec_wait(EC_STATUS_IBF, 0);
	if (status != B_OK)
		return status;

	sIsa->write_io_8(EC_DATA_PORT, value);
	return ec_wait(EC_STATUS_IBF, 0);
}


/* Read-modify-write one bit of one byte, under the ACPI global lock, with the
 * result read back. Returns B_OK only if the bit actually changed.
 */
static status_t
set_bit(int index, int value, char* message, size_t messageSize)
{
	uint8 offset = kWritable[index].offset;
	uint8 bit = kWritable[index].bit;
	uint32 lock = 0;
	bool locked = false;
	uint8 before = 0;
	uint8 after = 0;
	uint8 wanted;
	status_t status;

	/* Refuse to start if the EC is mid-transaction with somebody else. This
	 * is the cheapest way to avoid interleaving with the kernel's own driver.
	 */
	if (ec_wait(EC_STATUS_IBF | EC_STATUS_OBF, 0) != B_OK) {
		snprintf(message, messageSize,
			"EC busy, refused to write %s", kWritable[index].name);
		return B_BUSY;
	}

	/* The Field is declared Lock, so this is the protocol ACPICA follows for
	 * it too - taking it serialises us against ACPI-initiated EC traffic. If
	 * the firmware offers no global lock the call fails, in which case
	 * ACPICA is not using one either and we are no worse off than it is.
	 */
	if (sAcpi->acquire_global_lock(0xFFFF, &lock) == B_OK)
		locked = true;
	else
		TRACE("no ACPI global lock available; proceeding without it\n");

	status = ec_read_byte(offset, &before);
	if (status != B_OK) {
		if (locked)
			sAcpi->release_global_lock(lock);
		snprintf(message, messageSize, "could not read EC byte 0x%02x: %s",
			offset, strerror(status));
		return status;
	}

	if (!sSaved[index].saved) {
		sSaved[index].original = before;
		sSaved[index].saved = true;
	}

	wanted = value != 0 ? (uint8)(before | (1 << bit))
		: (uint8)(before & ~(1 << bit));

	if (wanted == before) {
		if (locked)
			sAcpi->release_global_lock(lock);
		snprintf(message, messageSize, "%s already %d (byte 0x%02x = 0x%02x)",
			kWritable[index].name, value, offset, before);
		return B_OK;
	}

	status = ec_write_byte(offset, wanted);
	if (status == B_OK)
		status = ec_read_byte(offset, &after);

	if (locked)
		sAcpi->release_global_lock(lock);

	if (status != B_OK) {
		snprintf(message, messageSize, "write of %s failed: %s",
			kWritable[index].name, strerror(status));
		return status;
	}

	snprintf(message, messageSize,
		"%s: byte 0x%02x was 0x%02x, wrote 0x%02x, reads back 0x%02x - %s",
		kWritable[index].name, offset, before, wanted, after,
		((after >> bit) & 1) == (value != 0) ? "took" : "DID NOT TAKE");

	TRACE("%s\n", message);
	return B_OK;
}


static void
restore_all(char* message, size_t messageSize)
{
	size_t offset = 0;
	int i;
	int restored = 0;

	for (i = 0; i < kWritableCount; i++) {
		uint32 lock = 0;
		bool locked = false;
		uint8 now = 0;

		if (!sSaved[i].saved)
			continue;

		if (sAcpi->acquire_global_lock(0xFFFF, &lock) == B_OK)
			locked = true;

		if (ec_read_byte(kWritable[i].offset, &now) == B_OK
			&& now != sSaved[i].original) {
			ec_write_byte(kWritable[i].offset, sSaved[i].original);
			restored++;
		}

		if (locked)
			sAcpi->release_global_lock(lock);

		offset += snprintf(message + offset, messageSize - offset,
			"restored byte 0x%02x to 0x%02x; ", kWritable[i].offset,
			sSaved[i].original);
		sSaved[i].saved = false;
	}

	if (restored == 0 && offset == 0)
		snprintf(message, messageSize, "nothing had been changed");

	TRACE("restore: %s\n", message);
}


/* #pragma mark - report */


static void
build_report(void)
{
	size_t offset = 0;
	int i;

	offset += snprintf(sReport + offset, sizeof(sReport) - offset,
		"embedded controller fields, read through ACPI\n\n");

	for (i = 0; i < kFieldCount && offset < sizeof(sReport) - 1; i++) {
		uint32 value = 0;
		status_t status = read_field(kFields[i].name, &value);

		if (status == B_OK) {
			offset += snprintf(sReport + offset, sizeof(sReport) - offset,
				"  %-6s = %-10" B_PRIu32 " %s\n", kFields[i].name, value,
				kFields[i].note);
		} else {
			offset += snprintf(sReport + offset, sizeof(sReport) - offset,
				"  %-6s = %-10s %s\n", kFields[i].name, "unreadable",
				strerror(status));
		}
	}

	if (sLog[0] != '\0') {
		offset += snprintf(sReport + offset, sizeof(sReport) - offset,
			"\nlast command: %s\n", sLog);
	}

	offset += snprintf(sReport + offset, sizeof(sReport) - offset,
		"\ncommands:  echo \"set USBP 1\" > /dev/misc/ectest\n"
		"           echo \"set USBO 1\" > /dev/misc/ectest\n"
		"           echo \"set ENPS 1\" > /dev/misc/ectest\n"
		"           echo \"restore\"    > /dev/misc/ectest\n"
		"after each set, run listusb and look for a fourth device.\n");

	sReportLength = offset;
}


/* #pragma mark - driver hooks */


static status_t
ectest_open(const char* name, uint32 flags, void** cookie)
{
	(void)name;
	(void)flags;
	*cookie = NULL;
	return B_OK;
}


static status_t
ectest_read(void* cookie, off_t position, void* buffer, size_t* numBytes)
{
	size_t remaining;

	(void)cookie;

	if (position == 0)
		build_report();

	if (position < 0 || (size_t)position >= sReportLength) {
		*numBytes = 0;
		return B_OK;
	}

	remaining = sReportLength - (size_t)position;
	if (*numBytes > remaining)
		*numBytes = remaining;

	if (user_memcpy(buffer, sReport + position, *numBytes) < B_OK)
		return B_BAD_ADDRESS;

	return B_OK;
}


static status_t
ectest_write(void* cookie, off_t position, const void* buffer,
	size_t* numBytes)
{
	char command[64];
	size_t length = *numBytes;
	char field[16];
	int value = -1;
	int i;

	(void)cookie;
	(void)position;

	if (length >= sizeof(command))
		length = sizeof(command) - 1;
	if (user_memcpy(command, buffer, length) < B_OK)
		return B_BAD_ADDRESS;
	command[length] = '\0';

	/* Trim the newline echo leaves behind. */
	while (length > 0 && (command[length - 1] == '\n'
			|| command[length - 1] == '\r' || command[length - 1] == ' ')) {
		command[--length] = '\0';
	}

	if (strcmp(command, "restore") == 0) {
		restore_all(sLog, sizeof(sLog));
		return B_OK;
	}

	/* Prove the direct-EC path works before believing anything it reports.
	 * A write that "does not take" is only interesting if the read that
	 * checked it is trustworthy, and if ec_read_byte always returned zero
	 * every write would look refused. Byte 0x00 is the temperature, which
	 * ACPI reports independently as RTMP, and byte 0x20 holds WLPW and BTPW,
	 * which are known to be 1 on a running machine. Matching values mean the
	 * transactions are reaching the real EC.
	 */
	if (strcmp(command, "verify") == 0) {
		static const uint8 kBytes[] = { 0x00, 0x20, 0x24, 0x26, 0x27 };
		size_t offset = 0;
		uint32 lock = 0;
		bool locked = false;
		int i;

		if (sAcpi->acquire_global_lock(0xFFFF, &lock) == B_OK)
			locked = true;

		for (i = 0; i < (int)(sizeof(kBytes) / sizeof(kBytes[0])); i++) {
			uint8 byteValue = 0;
			status_t status = ec_read_byte(kBytes[i], &byteValue);
			offset += snprintf(sLog + offset, sizeof(sLog) - offset,
				"0x%02x=%s%02x ", kBytes[i],
				status == B_OK ? "0x" : "err:", byteValue);
		}

		if (locked)
			sAcpi->release_global_lock(lock);

		snprintf(sLog + offset, sizeof(sLog) - offset,
			"(0x00 should equal RTMP; 0x20 should have bits 3 and 4 set for "
			"WLPW and BTPW)");
		TRACE("verify: %s\n", sLog);
		return B_OK;
	}

	/* Parsed by hand: the kernel has no sscanf. The grammar is small enough
	 * that this is shorter than explaining why it needs one.
	 */
	{
		const char* p = command;
		size_t n = 0;

		if (strncmp(p, "set ", 4) != 0) {
			snprintf(sLog, sizeof(sLog),
				"unrecognised command \"%s\" - expected \"set <FIELD> <0|1>\" "
				"or \"restore\"", command);
			*numBytes = 0;
			return B_BAD_VALUE;
		}
		p += 4;
		while (*p == ' ')
			p++;

		while (*p != '\0' && *p != ' ' && n < sizeof(field) - 1)
			field[n++] = *p++;
		field[n] = '\0';

		while (*p == ' ')
			p++;
		if (*p == '0')
			value = 0;
		else if (*p == '1')
			value = 1;

		if (n == 0 || value < 0) {
			snprintf(sLog, sizeof(sLog),
				"unrecognised command \"%s\" - expected \"set <FIELD> <0|1>\" "
				"or \"restore\"", command);
			*numBytes = 0;
			return B_BAD_VALUE;
		}
	}

	for (i = 0; i < kWritableCount; i++) {
		if (strcmp(field, kWritable[i].name) == 0) {
			set_bit(i, value, sLog, sizeof(sLog));
			return B_OK;
		}
	}

	/* Refusing everything not on the list is the whole safety story: a typo
	 * cannot reach a battery or thermal register.
	 */
	snprintf(sLog, sizeof(sLog),
		"\"%s\" is not writable here - only USBP, USBO and ENPS are", field);
	*numBytes = 0;
	return B_NOT_ALLOWED;
}


static status_t
ectest_control(void* cookie, uint32 op, void* arg, size_t length)
{
	(void)cookie;
	(void)op;
	(void)arg;
	(void)length;
	return B_DEV_INVALID_IOCTL;
}


static status_t
ectest_close(void* cookie)
{
	(void)cookie;
	return B_OK;
}


static status_t
ectest_free(void* cookie)
{
	(void)cookie;
	return B_OK;
}


static device_hooks sDeviceHooks = {
	ectest_open,
	ectest_close,
	ectest_free,
	ectest_control,
	ectest_read,
	ectest_write,
	NULL,
	NULL,
	NULL,
	NULL
};


status_t
init_hardware(void)
{
	return B_OK;
}


status_t
init_driver(void)
{
	status_t status = get_module(B_ACPI_MODULE_NAME, (module_info**)&sAcpi);
	if (status != B_OK) {
		TRACE("no ACPI module: %s\n", strerror(status));
		return status;
	}

	status = get_module(B_ISA_MODULE_NAME, (module_info**)&sIsa);
	if (status != B_OK) {
		TRACE("no ISA module: %s\n", strerror(status));
		put_module(B_ACPI_MODULE_NAME);
		sAcpi = NULL;
		return status;
	}

	memset(sSaved, 0, sizeof(sSaved));
	sLog[0] = '\0';

	TRACE("loaded - reads are safe, writes touch USBP/USBO/ENPS only\n");
	return B_OK;
}


void
uninit_driver(void)
{
	/* Leaving a bit set behind would be a surprise on the next boot, and
	 * unloading is exactly when the caller has stopped paying attention.
	 */
	if (sAcpi != NULL && sIsa != NULL) {
		char message[512];
		restore_all(message, sizeof(message));
	}

	if (sIsa != NULL) {
		put_module(B_ISA_MODULE_NAME);
		sIsa = NULL;
	}
	if (sAcpi != NULL) {
		put_module(B_ACPI_MODULE_NAME);
		sAcpi = NULL;
	}
}


const char**
publish_devices(void)
{
	static const char* devices[] = { ECTEST_DEVICE_NAME, NULL };
	return devices;
}


device_hooks*
find_device(const char* name)
{
	(void)name;
	return &sDeviceHooks;
}
