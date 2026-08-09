// Run SiParser over a transport stream and print the service names it finds.
//
// The GUI proves nothing that can be checked from a shell, and the channel
// naming path has two halves that can each be wrong in ways the other hides:
// the SDT section parsing, and the ARIB STD-B24 text decoding. This exercises
// both against a file and prints the result as UTF-8.
//
// Feed it tools/make_test_ts.py's output and the names printed here should be
// exactly the ones that script says it encoded.
//
// Build:  setarch x86 g++ -o test_si tools/test_si.cpp src/SiParser.cpp \
//             src/AribText.cpp -lstdc++ -lsupc++
// Run:    ./test_si oneseg-test.ts

#include <stdio.h>
#include <stdlib.h>

#include "../src/SiParser.h"

int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s FILE.ts\n", argv[0]);
		return 1;
	}

	FILE* file = fopen(argv[1], "rb");
	if (file == NULL) {
		fprintf(stderr, "cannot open %s\n", argv[1]);
		return 1;
	}

	SiParser parser;

	// Deliberately not a multiple of 188: a tuner hands over whatever a USB
	// transfer happened to contain, so sections routinely straddle chunk
	// boundaries and the reassembly has to cope. A test that fed neat
	// packet-aligned blocks would not exercise that at all.
	const size_t kChunk = 4099;
	unsigned char buffer[kChunk];
	size_t total = 0;
	int firstHit = -1;

	while (true) {
		size_t got = fread(buffer, 1, kChunk, file);
		if (got == 0)
			break;
		if (parser.Feed(buffer, got) && firstHit < 0)
			firstHit = (int)total;
		total += got;
	}
	fclose(file);

	printf("read %zu bytes\n", total);
	if (firstHit >= 0)
		printf("first name completed after %d bytes\n", firstHit);

	const std::map<uint16, std::string>& services = parser.Services();
	if (services.empty()) {
		printf("no service names found\n");
		return 1;
	}

	printf("%zu service(s):\n", services.size());
	for (std::map<uint16, std::string>::const_iterator it = services.begin();
			it != services.end(); ++it) {
		printf("  0x%04x  %s\n", it->first, it->second.c_str());
	}
	printf("primary: %s\n", parser.PrimaryName().c_str());
	return 0;
}
