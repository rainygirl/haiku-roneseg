#include "DiagnosticWindow.h"

#include <Button.h>
#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Font.h>
#include <LayoutBuilder.h>
#include <Path.h>
#include <ScrollView.h>
#include <Messenger.h>
#include <TextView.h>

#include <stdio.h>

#include "ChannelTable.h"
#include "TunerDiagnostic.h"
#include "UsbTuner.h"

namespace {

const uint32 kMsgLine = 'dLin';
const uint32 kMsgFinished = 'dFin';
const uint32 kMsgClose = 'dCls';

} // namespace


DiagnosticWindow::DiagnosticWindow(BRect frame, UsbTuner* tuner)
	:
	BWindow(frame, "チューナー診断", B_TITLED_WINDOW,
		B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS),
	fTuner(tuner),
	fText(NULL),
	fCloseButton(NULL),
	fThread(-1),
	fCancel(false)
{
	fText = new BTextView("report");
	fText->MakeEditable(false);
	fText->SetWordWrap(false);
	// The per-channel table lines up only in a fixed-width font.
	fText->SetFontAndColor(be_fixed_font);
	BScrollView* scroller = new BScrollView("scroller", fText, 0, true, true);
	scroller->SetExplicitMinSize(BSize(560, 360));

	fCloseButton = new BButton("close", "閉じる", new BMessage(kMsgClose));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_SMALL_SPACING)
		.Add(scroller)
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fCloseButton)
		.End()
	.End();

	fThread = spawn_thread(RunEntry, "roneseg diagnostic", B_LOW_PRIORITY,
		this);
	if (fThread >= 0)
		resume_thread(fThread);
}


DiagnosticWindow::~DiagnosticWindow()
{
}


void
DiagnosticWindow::Line(const std::string& line, void* cookie)
{
	DiagnosticWindow* window = static_cast<DiagnosticWindow*>(cookie);
	if (window->fCancel)
		return;
	BMessage message(kMsgLine);
	message.AddString("line", line.c_str());
	// With a deadline: if the window is closing and its queue is full, a
	// blocking post would stall this thread while QuitRequested waits on it.
	BMessenger(window).SendMessage(&message, (BHandler*)NULL, 500000);
}


status_t
DiagnosticWindow::RunEntry(void* cookie)
{
	DiagnosticWindow* window = static_cast<DiagnosticWindow*>(cookie);
	std::vector<int32> channels;
	for (int32 c = ChannelTable::kFirstChannel; c <= ChannelTable::kLastChannel;
			c++) {
		channels.push_back(c);
	}
	char layout[96];
	snprintf(layout, sizeof(layout), "frequency word: 0x%02x/0x%02x, latch 0x%02x",
		window->fTuner->FrequencyRegister(),
		window->fTuner->FrequencyRegisterLow(), window->fTuner->LatchValue());
	Line(layout, window);
	TunerDiagnostic diagnostic(window->fTuner, Line, window);
	diagnostic.Run(channels, &window->fCancel);
	if (!window->fCancel) {
		BMessage finished(kMsgFinished);
		BMessenger(window).SendMessage(&finished, (BHandler*)NULL, 500000);
	}
	return B_OK;
}


void
DiagnosticWindow::SaveReport()
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK
		|| path.Append("roneseg") != B_OK)
		return;
	create_directory(path.Path(), 0755);
	path.Append("diagnosis.txt");
	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() != B_OK)
		return;
	file.Write(fReport.c_str(), fReport.size());
	std::string note = std::string("\nsaved to ") + path.Path() + "\n";
	fText->Insert(fText->TextLength(), note.c_str(), note.size());
}


void
DiagnosticWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgLine:
		{
			const char* line = NULL;
			if (message->FindString("line", &line) == B_OK) {
				std::string text = std::string(line) + "\n";
				fReport += text;
				fText->Insert(fText->TextLength(), text.c_str(), text.size());
				fText->ScrollToOffset(fText->TextLength());
			}
			break;
		}

		case kMsgFinished:
			fThread = -1;
			SaveReport();
			break;

		case kMsgClose:
			PostMessage(B_QUIT_REQUESTED);
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


bool
DiagnosticWindow::QuitRequested()
{
	// Stop between channels and wait: the thread is using the tuner, and the
	// window must not go away underneath its PostMessage calls.
	if (fThread >= 0) {
		fCancel = true;
		status_t result;
		wait_for_thread(fThread, &result);
		fThread = -1;
	}
	return true;
}
