#include "SiParser.h"

#include <string.h>

#include "AribText.h"

namespace {

const size_t kPacketSize = 188;
const uint16 kSdtPid = 0x0011;
const uint8 kSdtActualTableId = 0x42;
const uint8 kServiceDescriptorTag = 0x48;


uint32
Crc32Mpeg(const uint8* data, size_t size)
{
	uint32 crc = 0xFFFFFFFF;
	for (size_t i = 0; i < size; i++) {
		crc ^= (uint32)data[i] << 24;
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc & 0x80000000) != 0
				? (crc << 1) ^ 0x04C11DB7 : crc << 1;
		}
	}
	return crc;
}

} // namespace


SiParser::SiParser()
	:
	fPendingLength(0),
	fSawNewName(false)
{
}


void
SiParser::Reset()
{
	fServices.clear();
	fPending.clear();
	fPendingLength = 0;
}


std::string
SiParser::PrimaryName() const
{
	if (fServices.empty())
		return std::string();
	return fServices.begin()->second;
}


bool
SiParser::Feed(const uint8* data, size_t size)
{
	fSawNewName = false;

	// Find the packet rhythm rather than assuming the chunk starts on one:
	// a tuner hands over whatever the USB transfer happened to contain.
	size_t i = 0;
	while (i + kPacketSize <= size) {
		if (data[i] != 0x47) {
			i++;
			continue;
		}
		HandlePacket(data + i);
		i += kPacketSize;
	}

	return fSawNewName;
}


void
SiParser::HandlePacket(const uint8* packet)
{
	uint16 pid = ((packet[1] & 0x1F) << 8) | packet[2];
	if (pid != kSdtPid)
		return;

	if ((packet[1] & 0x80) != 0)	// transport_error_indicator
		return;

	bool payloadStart = (packet[1] & 0x40) != 0;
	uint8 adaptationControl = (packet[3] >> 4) & 0x03;
	if ((adaptationControl & 0x01) == 0)
		return;						// no payload

	size_t offset = 4;
	if ((adaptationControl & 0x02) != 0) {
		uint8 adaptationLength = packet[4];
		offset = 5 + adaptationLength;
		if (offset >= kPacketSize)
			return;
	}

	if (payloadStart) {
		// pointer_field: how far past it the new section begins. Bytes before
		// that belong to the previous section, which we drop - a section
		// split across the start of a capture cannot be completed anyway.
		uint8 pointer = packet[offset];
		offset += 1 + pointer;
		if (offset >= kPacketSize)
			return;

		fPending.assign((const char*)packet + offset, kPacketSize - offset);
		fPendingLength = 0;
		if (fPending.size() >= 3) {
			fPendingLength = 3
				+ (((uint8)fPending[1] & 0x0F) << 8 | (uint8)fPending[2]);
		}
	} else {
		if (fPending.empty())
			return;					// continuation with no start; ignore
		fPending.append((const char*)packet + offset, kPacketSize - offset);
		if (fPendingLength == 0 && fPending.size() >= 3) {
			fPendingLength = 3
				+ (((uint8)fPending[1] & 0x0F) << 8 | (uint8)fPending[2]);
		}
	}

	if (fPendingLength != 0 && fPending.size() >= fPendingLength) {
		ParseSection((const uint8*)fPending.data(), fPendingLength);
		fPending.clear();
		fPendingLength = 0;
	}
}


bool
SiParser::ParseSection(const uint8* section, size_t size)
{
	if (size < 15)
		return false;
	if (section[0] != kSdtActualTableId)
		return false;

	// A corrupt section would otherwise put nonsense in the channel list, and
	// on a weak signal corrupt sections are the normal case rather than the
	// exception - which is exactly why PSI carries a CRC.
	if (Crc32Mpeg(section, size) != 0)
		return false;

	if ((section[5] & 0x01) == 0)	// current_next_indicator
		return false;

	// table_id(1) length(2) tsid(2) version(1) section(1) last(1) onid(2)
	// reserved(1) = 11 bytes before the service loop, 4 bytes of CRC after.
	size_t i = 11;
	size_t end = size - 4;

	while (i + 5 <= end) {
		uint16 serviceId = (section[i] << 8) | section[i + 1];
		size_t loopLength = ((section[i + 3] & 0x0F) << 8) | section[i + 4];
		i += 5;
		if (i + loopLength > end)
			break;

		size_t descriptorEnd = i + loopLength;
		while (i + 2 <= descriptorEnd) {
			uint8 tag = section[i];
			uint8 length = section[i + 1];
			if (i + 2 + length > descriptorEnd)
				break;

			if (tag == kServiceDescriptorTag && length >= 3) {
				const uint8* body = section + i + 2;
				uint8 providerLength = body[1];
				if ((size_t)2 + providerLength < (size_t)length) {
					uint8 nameLength = body[2 + providerLength];
					const uint8* name = body + 3 + providerLength;
					if ((size_t)3 + providerLength + nameLength
							<= (size_t)length) {
						std::string decoded = AribText::ToUtf8(name,
							nameLength);
						if (!decoded.empty()
							&& fServices[serviceId] != decoded) {
							fServices[serviceId] = decoded;
							fSawNewName = true;
						}
					}
				}
			}

			i += 2 + length;
		}

		i = descriptorEnd;
	}

	return true;
}
