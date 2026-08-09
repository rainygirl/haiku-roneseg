#ifndef RONESEG_CHANNEL_TABLE_H
#define RONESEG_CHANNEL_TABLE_H

#include <SupportDefs.h>

#include <string>
#include <vector>

// The Japanese ISDB-T terrestrial channel plan.
//
// This is the only plan a One-Seg tuner is any use against. Korea's
// terrestrial broadcasts are ATSC 1.0 (8VSB) and Korea's mobile TV is T-DMB
// on VHF Band III - neither is reachable by this hardware, at the RF front
// end or at the demodulator. See README.md, "What this cannot do".
namespace ChannelTable {

struct Channel {
	int32		physical;		// UHF channel number, 13..52
	uint64		frequencyHz;	// centre frequency

	std::string Label() const;
};

// Japan's digital terrestrial allocation is UHF 13-52 (470-710 MHz). It was
// 13-62 until the 700 MHz band was reallocated after the 2011 analogue
// shutdown; nothing broadcasts above 52 now, so scanning there only wastes
// time on a machine this slow.
const int32 kFirstChannel = 13;
const int32 kLastChannel = 52;

// f(ch) = 473 + 1/7 + (ch - 13) * 6 MHz. The 1/7 MHz offset is part of the
// ISDB-T plan, not a rounding artefact - dropping it puts the carrier
// 142.857 kHz off, which is a third of a One-Seg segment's whole width.
uint64 FrequencyFor(int32 physicalChannel);

std::vector<Channel> All();

} // namespace ChannelTable

#endif
