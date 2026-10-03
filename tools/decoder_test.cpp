// Offline end-to-end local IPC test, without opening a tuner or using RF.
// setarch x86 g++ -D_GLIBCXX_USE_CXX11_ABI=1 -O2 -Isrc tools/decoder_test.cpp src/LeiraDecoder.cpp \
//   src/UsbTuner.cpp -ldevice -lbe -lnetwork -o decoder_test
#include "LeiraDecoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv)
{
	if (argc != 4) {
		fprintf(stderr, "usage: decoder_test SESSION.json CAPTURE.enc OUTPUT.ts\n");
		return 2;
	}
	FILE* input = fopen(argv[2], "rb");
	FILE* output = fopen(argv[3], "wb");
	if (!input || !output) return 2;
	LeiraDecoder decoder(NULL, argv[1]);
	bigtime_t started = system_time();
	status_t status = decoder.Initialize();
	fprintf(stderr, "initialize: %.3f s\n", (system_time() - started) / 1000000.0);
	if (status != B_OK) {
		fprintf(stderr, "%s\n", decoder.Error().c_str());
		return 1;
	}
	uint8* data = new uint8[4096 * 208];
	std::vector<uint8> plain;
	size_t total = 0;
	started = system_time();
	while (size_t size = fread(data, 1, 4096 * 208, input)) {
		status = decoder.Decode(data, size, plain);
		if (status != B_OK) break;
		if (!plain.empty() && fwrite(&plain[0], 1, plain.size(), output) != plain.size()) {
			status = B_IO_ERROR;
			break;
		}
		total += plain.size();
	}
	delete[] data;
	if (ferror(input) || fclose(output) != 0) status = B_IO_ERROR;
	fclose(input);
	fprintf(stderr, "decode: %lu bytes, %.3f s, status %ld %s\n",
		(unsigned long)total, (system_time() - started) / 1000000.0,
		(long)status, decoder.Error().c_str());
	return status == B_OK ? 0 : 1;
}
