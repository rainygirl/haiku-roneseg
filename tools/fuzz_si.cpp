// Adversarial input into the two parsers that read broadcast-authored data.
//
// Everything else in this app reads bytes the machine produced. SiParser and
// AribText read bytes a transmitter produced, over an aerial, on a netbook
// that will frequently be indoors and half a prefecture from the mast. Corrupt
// sections are not the exceptional case there, they are Tuesday - and both
// parsers run on the tuner's own thread, so a hang in either freezes playback
// rather than producing a bad string.
//
// SiParser has a CRC check that throws out damaged sections, which is exactly
// why the interesting failures are the ones that happen *before* the CRC is
// reached: section length arithmetic, descriptor loop bounds, and the ARIB
// state machine's escape measurement.
//
// Runs anywhere with an iconv - no hardware, no Media Kit - which is the
// point: it can be run while the machine is off. Off Haiku, supply a
// SupportDefs.h that typedefs uint8/uint16/uint32/int32 and build with -I
// pointing at it.
//
//   clang++ -fsanitize=address,undefined -I<shim> -o fuzz_si \
//       tools/fuzz_si.cpp src/AribText.cpp src/SiParser.cpp -liconv
//   ./fuzz_si
//
// The sanitizers are the point; without them this only catches hangs and
// hard crashes, and the out-of-bounds reads it is really looking for would
// pass silently.

#include "../src/AribText.h"
#include "../src/SiParser.h"

#include <stdio.h>
#include <stdlib.h>

#include <vector>

// Fixed seed: a fuzzer that finds a crash only on some runs is not something
// you can bisect against.
static const unsigned kSeed = 12345;
static const int kTextTrials = 20000;
static const int kStreamTrials = 300;


int
main()
{
	srandom(kSeed);

	// Arbitrary bytes. Mostly lands in the two-byte kanji path.
	for (int trial = 0; trial < kTextTrials; trial++) {
		size_t size = random() % 300;
		std::vector<uint8> data(size);
		for (size_t i = 0; i < size; i++)
			data[i] = random() & 0xFF;
		AribText::ToUtf8(size > 0 ? &data[0] : NULL, size);
	}
	printf("random bytes:        %d inputs\n", kTextTrials);

	// Bytes drawn only from the alphabet the state machine reacts to, so
	// nearly every byte takes a control path. This is where a mis-measured
	// escape sequence would leave the index unadvanced and spin forever.
	static const uint8 kControlBytes[] = {
		0x1B,							// ESC
		0x24, 0x28, 0x29, 0x2A, 0x2B,	// designation intermediates
		0x30, 0x31, 0x42, 0x4A,			// final bytes
		0x19, 0x1D,						// SS2, SS3
		0x0E, 0x0F,						// LS1, LS0
		0x22							// an ordinary payload byte
	};
	for (int trial = 0; trial < kTextTrials; trial++) {
		size_t size = random() % 200;
		std::vector<uint8> data(size);
		for (size_t i = 0; i < size; i++)
			data[i] = kControlBytes[random() % sizeof(kControlBytes)];
		AribText::ToUtf8(size > 0 ? &data[0] : NULL, size);
	}
	printf("escape-heavy bytes:  %d inputs\n", kTextTrials);

	// Transport packets with valid sync and SDT PID but garbage payload -
	// what a marginal signal actually delivers, and the only way to reach
	// SiParser's section reassembly with lengths it did not choose.
	for (int trial = 0; trial < kStreamTrials; trial++) {
		SiParser parser;
		std::vector<uint8> stream(188 * 40);
		for (size_t i = 0; i < stream.size(); i++)
			stream[i] = random() & 0xFF;
		for (size_t i = 0; i + 188 <= stream.size(); i += 188) {
			stream[i] = 0x47;
			stream[i + 1] = 0x40;	// payload_unit_start, PID 0x0011
			stream[i + 2] = 0x11;
			stream[i + 3] = 0x10;	// payload only
		}
		parser.Feed(&stream[0], stream.size());
	}
	printf("corrupt SDT packets: %d streams\n", kStreamTrials);

	printf("\nall survived\n");
	return 0;
}
