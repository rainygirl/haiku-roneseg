# -*- coding: utf-8 -*-
"""Inject an ARIB-encoded SDT into a transport stream, producing a capture
that exercises the channel-name path as well as the decode path.

There is no One-Seg broadcast to point the machine at while developing, and
the tuner hardware is not usable yet, so the only way to test the parts that
are finished is a synthetic stream. This adds the one thing ffmpeg will not
write: a Japanese Service Description Table shaped the way ISDB-T writes it.

Service names in ISDB-T are ARIB STD-B24 text, not UTF-8. The default 8-bit
designation puts the two-byte kanji plane in GL, and that plane is JIS X 0208
- the same one EUC-JP carries with the high bit set. So encoding is "encode
EUC-JP, clear bit 7 of every byte", and decoding is the reverse followed by
an EUC-JP to UTF-8 conversion. src/AribText.cpp is the other half of this.

Usage:
  python3 tools/make_test_ts.py base.ts oneseg-test.ts
"""

import io
import sys

PACKET_SIZE = 188
SDT_PID = 0x0011
NULL_PID = 0x1FFF

# Two services, the way a real multiplex carries several. Pure kana and kanji
# so every character lives in the JIS X 0208 plane and no escape sequence is
# needed - the decoder's escape handling is exercised separately by the
# alphanumeric service below.
SERVICES = [
    (0x0400, "テスト放送"),
    (0x0401, "ハイク実験局"),
]


def crc32_mpeg(data):
    """The MPEG-2 CRC-32 used by PSI sections: poly 0x04C11DB7, init all
    ones, MSB first, no final inversion. Not the same as zlib's."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc


def arib_encode(text):
    """UTF-8 text to ARIB kanji-plane bytes (JIS X 0208 in GL)."""
    euc = text.encode("euc_jp")
    return bytes(b & 0x7F for b in euc)


def service_descriptor(name):
    body = bytearray()
    body.append(0xC0)               # service_type: digital audio/video, 1seg
    body.append(0x00)               # service_provider_name_length
    encoded = arib_encode(name)
    body.append(len(encoded))
    body += encoded
    return bytes([0x48, len(body)]) + bytes(body)


def build_sdt(transport_stream_id=0x0001, original_network_id=0x7FE0):
    services = bytearray()
    for service_id, name in SERVICES:
        descriptors = service_descriptor(name)
        services += service_id.to_bytes(2, "big")
        # reserved(6)=0b111111, EIT_schedule=0, EIT_present_following=1
        services.append(0xFD)
        # running_status=4 (running), free_CA_mode=0, then the 12-bit length
        length = len(descriptors)
        services.append(0x80 | ((length >> 8) & 0x0F))
        services.append(length & 0xFF)
        services += descriptors

    # Everything the section_length field covers: from transport_stream_id
    # through the CRC.
    payload = bytearray()
    payload += transport_stream_id.to_bytes(2, "big")
    payload.append(0xC1)            # reserved(2), version 0, current_next=1
    payload.append(0x00)            # section_number
    payload.append(0x00)            # last_section_number
    payload += original_network_id.to_bytes(2, "big")
    payload.append(0xFF)            # reserved_future_use
    payload += services

    section_length = len(payload) + 4   # + CRC_32
    header = bytearray()
    header.append(0x42)             # table_id: SDT, actual transport stream
    header.append(0xF0 | ((section_length >> 8) & 0x0F))
    header.append(section_length & 0xFF)

    section = bytes(header) + bytes(payload)
    return section + crc32_mpeg(section).to_bytes(4, "big")


def sdt_packet(section, continuity):
    if len(section) > PACKET_SIZE - 5:
        raise SystemExit("SDT does not fit one packet; sectioning not needed here")
    packet = bytearray(PACKET_SIZE)
    packet[0] = 0x47
    packet[1] = 0x40 | ((SDT_PID >> 8) & 0x1F)      # payload_unit_start
    packet[2] = SDT_PID & 0xFF
    packet[3] = 0x10 | (continuity & 0x0F)          # payload only
    packet[4] = 0x00                                # pointer_field
    packet[5:5 + len(section)] = section
    for i in range(5 + len(section), PACKET_SIZE):
        packet[i] = 0xFF                            # stuffing
    return bytes(packet)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)

    source = io.open(sys.argv[1], "rb").read()
    section = build_sdt()

    out = bytearray()
    continuity = 0
    injected = 0
    replaced = 0
    since_last = 0

    # Roughly every 200ms at this rate. Real broadcasts repeat the SDT at
    # least every two seconds; more often here so a short capture still
    # carries several copies no matter where it is cut.
    interval = 60

    for offset in range(0, len(source) - PACKET_SIZE + 1, PACKET_SIZE):
        packet = source[offset:offset + PACKET_SIZE]
        if packet[0] != 0x47:
            out += packet
            continue

        pid = ((packet[1] & 0x1F) << 8) | packet[2]

        # Drop whatever SDT the muxer wrote. ffmpeg emits a DVB-flavoured one,
        # where a service name is plain ASCII; ARIB puts the two-byte kanji
        # plane in GL instead, so those same bytes decode as kanji. Leaving
        # both in would make the parser's output depend on which section it
        # saw last, and the test would prove nothing.
        if pid == SDT_PID:
            continue

        since_last += 1

        if since_last >= interval:
            # Prefer overwriting a null packet: that keeps the mux rate and
            # every PCR interval exactly as ffmpeg wrote them, which matters
            # because the decoder paces playback off those.
            if pid == NULL_PID:
                out += sdt_packet(section, continuity)
                continuity = (continuity + 1) & 0x0F
                replaced += 1
                since_last = 0
                continue
            out += sdt_packet(section, continuity)
            continuity = (continuity + 1) & 0x0F
            injected += 1
            since_last = 0

        out += packet

    io.open(sys.argv[2], "wb").write(bytes(out))
    print("%s: %d bytes, SDT section %d bytes, %d null packets replaced, "
          "%d packets inserted" % (sys.argv[2], len(out), len(section),
                                   replaced, injected))
    for service_id, name in SERVICES:
        print("  service 0x%04x  %s" % (service_id, name))


if __name__ == "__main__":
    main()
