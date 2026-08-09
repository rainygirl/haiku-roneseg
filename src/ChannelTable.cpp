#include "ChannelTable.h"

#include <stdio.h>

namespace ChannelTable {

std::string
Channel::Label() const
{
	char buffer[64];
	// Broadcast convention quotes the channel number and the frequency in
	// MHz to three decimals; the 1/7 MHz offset makes the third decimal
	// meaningful (473.143, 479.143, ...).
	snprintf(buffer, sizeof(buffer), "UHF %" B_PRId32 "  %.3f MHz",
		physical, frequencyHz / 1000000.0);
	return std::string(buffer);
}


uint64
FrequencyFor(int32 physicalChannel)
{
	// 473 MHz + 1/7 MHz for channel 13, then 6 MHz per channel. Kept in Hz
	// and integer throughout: 1000000 / 7 truncates to 142857 Hz, which is
	// 1/7 Hz low - far inside any tuner's step size, and exact enough that
	// every channel lands on the same value the plan lists.
	const uint64 kChannel13 = 473000000ULL + 1000000ULL / 7ULL;
	return kChannel13 + static_cast<uint64>(physicalChannel - 13) * 6000000ULL;
}


std::vector<Channel>
All()
{
	std::vector<Channel> channels;
	for (int32 ch = kFirstChannel; ch <= kLastChannel; ch++) {
		Channel channel;
		channel.physical = ch;
		channel.frequencyHz = FrequencyFor(ch);
		channels.push_back(channel);
	}
	return channels;
}

} // namespace ChannelTable
