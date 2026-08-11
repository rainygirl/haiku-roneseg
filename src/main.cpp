#include <Application.h>

#include <stdio.h>
#include <string.h>

#include "MainWindow.h"
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
PrintUsage(const char* binary)
{
	printf(
		"Usage: %s [--play FILE.ts] [--list-usb]\n"
		"\n"
		"  --play FILE.ts   Replay a captured transport stream instead of\n"
		"                   using the tuner - the no-hardware path.\n"
		"  --list-usb       Print every USB device that could plausibly be a\n"
		"                   tuner, with its descriptors, and exit. Run this\n"
		"                   first on a machine whose tuner is unidentified.\n",
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
