#ifndef RONESEG_SI_PARSER_H
#define RONESEG_SI_PARSER_H

#include <SupportDefs.h>

#include <map>
#include <string>

// Pulls the service names out of a transport stream's Service Description
// Table, so channels can be listed by what they are called rather than by
// their UHF number.
//
// Fed the same bytes that go to the decoder, incrementally: a broadcaster
// repeats the SDT every couple of seconds precisely so a receiver that tunes
// in at any moment gets one, and a scan cannot know in advance how long that
// will take. Feed() may be called with arbitrary chunk boundaries; sections
// are reassembled across them.
//
// Only the SDT is parsed. The EIT would give programme titles too, but it is
// far larger, arrives more slowly, and is not what a channel list needs.
class SiParser {
public:
	SiParser();

	// Returns true when this chunk completed at least one new service name.
	bool Feed(const uint8* data, size_t size);

	// service_id -> UTF-8 name, in service_id order.
	const std::map<uint16, std::string>& Services() const { return fServices; }

	// The first service name seen, which is what a One-Seg multiplex's
	// channel is normally called. Empty until one arrives.
	std::string PrimaryName() const;

	void Reset();

private:
	void HandlePacket(const uint8* packet);
	bool ParseSection(const uint8* section, size_t size);

	std::map<uint16, std::string>	fServices;

	// Section reassembly for PID 0x0011.
	std::string	fPending;
	size_t		fPendingLength;	// 0 when not currently in a section
	bool		fSawNewName;
};

#endif
