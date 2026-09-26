#include <Application.h>

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "ChannelTable.h"
#include "MainWindow.h"
#include "TunerDiagnostic.h"
#include "UsbTuner.h"

static const char* kSignature = "application/x-vnd.ROneSeg";


class ROneSegApp : public BApplication {
public:
	explicit ROneSegApp(const std::string& capturePath)
		:
		BApplication(kSignature),
		fCapturePath(capturePath)
	{
	}

	virtual void ReadyToRun()
	{
		(new MainWindow(fCapturePath))->Show();
	}

private:
	std::string fCapturePath;
};


static void
PrintLine(const std::string& line, void*)
{
	printf("%s\n", line.c_str());
	fflush(stdout);
}


// "13-52", "20,27,33" or a mix; anything outside the UHF table is dropped.
static std::vector<int32>
ParseChannels(const char* spec)
{
	std::vector<int32> channels;
	std::string text(spec);
	size_t start = 0;
	while (start <= text.size()) {
		size_t comma = text.find(',', start);
		std::string part = text.substr(start, comma == std::string::npos
			? std::string::npos : comma - start);
		int from = 0;
		int to = 0;
		if (sscanf(part.c_str(), "%d-%d", &from, &to) == 2) {
		} else if (sscanf(part.c_str(), "%d", &from) == 1) {
			to = from;
		} else {
			from = 1;
			to = 0;
		}
		for (int c = from; c <= to; c++) {
			if (c >= ChannelTable::kFirstChannel
				&& c <= ChannelTable::kLastChannel)
				channels.push_back(c);
		}
		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}
	return channels;
}


static int
RunDiagnosis(const char* spec, const char* layout)
{
	std::vector<int32> channels = ParseChannels(spec);
	UsbTuner tuner;
	tuner.LoadSettings();
	// Optional frequency-word layout "HH,LL,LATCH" in hex, to compare the
	// candidates without touching the saved settings.
	unsigned high, low, latch;
	if (layout != NULL
		&& sscanf(layout, "%x,%x,%x", &high, &low, &latch) == 3) {
		tuner.SetFrequencyRegisters((uint8)high, (uint8)low);
		tuner.SetLatchValue((uint8)latch);
	}
	printf("frequency word: 0x%02x/0x%02x, latch 0x%02x\n",
		tuner.FrequencyRegister(), tuner.FrequencyRegisterLow(),
		tuner.LatchValue());
	TunerDiagnostic diagnostic(&tuner, PrintLine, NULL);
	bool ok = diagnostic.Run(channels, NULL);
	tuner.Close();
	return ok ? 0 : 1;
}


static void
PrintUsage(const char* binary)
{
	printf(
		"Usage: %s [--play FILE.ts] [--list-usb] [--diagnose [CHANNELS]]\n"
		"\n"
		"  --play FILE.ts   Replay a captured transport stream instead of\n"
		"                   using the tuner - the no-hardware path.\n"
		"  --list-usb       Print every USB device that could plausibly be a\n"
		"                   tuner, with its descriptors, and exit. Run this\n"
		"                   first on a machine whose tuner is unidentified.\n"
		"  --diagnose [CH]  Check the internal tuner stage by stage and print\n"
		"                   a report. CH is e.g. 13-52 (default) or 20,27.\n"
		"                   An optional HH,LL,LATCH (hex) overrides the\n"
		"                   frequency-word layout for this run only.\n",
		binary);
}


int
main(int argc, char** argv)
{
	std::string capturePath;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--list-usb") == 0) {
			// Before BApplication: this is a diagnostic, and it should work
			// on a machine where the GUI side has some other problem.
			printf("%s", UsbTuner::ScanReport().c_str());
			return 0;
		}
		if (strcmp(argv[i], "--diagnose") == 0) {
			// Headless tuner check; see TunerDiagnostic.h. Optional channel
			// list: "13-52" (default) or "20,27,33".
			const char* spec = i + 1 < argc ? argv[i + 1] : "13-52";
			const char* layout = i + 2 < argc ? argv[i + 2] : NULL;
			return RunDiagnosis(spec, layout);
		}
		if (strcmp(argv[i], "--play") == 0 && i + 1 < argc) {
			capturePath = argv[++i];
			continue;
		}
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			PrintUsage(argv[0]);
			return 0;
		}
		fprintf(stderr, "unrecognised argument: %s\n", argv[i]);
		PrintUsage(argv[0]);
		return 1;
	}

	ROneSegApp app(capturePath);
	app.Run();
	return 0;
}
