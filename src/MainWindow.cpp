#include "MainWindow.h"

#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <OS.h>
#include <String.h>
#include <TextControl.h>

#include <stdlib.h>
#include <ListItem.h>
#include <ListView.h>
#include <ScrollView.h>
#include <StringView.h>

#include <new>

#include "FileTuner.h"
#include "Player.h"
#include "TunerAdapterIO.h"
#include "UsbTuner.h"
#include "VideoView.h"

static const uint32 kMsgTune = 'Tune';
static const uint32 kMsgUsbReport = 'UsbR';
static const uint32 kMsgToggleScale = 'Scal';
static const uint32 kMsgStop = 'Stop';
static const uint32 kMsgScan = 'Scan';
static const uint32 kMsgScanHit = 'Shit';
static const uint32 kMsgScanDone = 'Sdon';
static const uint32 kMsgSettings = 'Setg';
static const uint32 kMsgApplySettings = 'AplS';
static const uint32 kMsgQuit = 'Quit';


MainWindow::MainWindow(const std::string& capturePath)
	:
	// 1600x768 is the whole panel on a VAIO P. Opening at half the width
	// leaves room for a Terminal beside it, which is where this app is going
	// to be used from for a long while yet.
	BWindow(BRect(60, 60, 60 + 780, 60 + 520), "R One-Seg",
		B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS),
	fChannels(ChannelTable::All()),
	fCapturePath(capturePath),
	fChannelList(NULL),
	fScanButton(NULL),
	fVideoView(NULL),
	fStatusView(NULL),
	fSettingsPanel(NULL),
	fSettingsField(NULL),
	fPlayer(NULL),
	fTuner(NULL),
	fScanThread(-1),
	fScanCancel(false),
	fQuitPending(false),
	fScanFirstHit(-1)
{
	BuildLayout();

	fPlayer = new Player(BMessenger(this), fVideoView);

	if (!fCapturePath.empty()) {
		fTuner = new FileTuner(fCapturePath.c_str());
		SetStatusText("replaying " + fCapturePath);
		// Naming a capture on the command line is already the instruction to
		// play it; posting rather than calling directly so this happens once
		// the window is running its own message loop, not during construction.
		PostMessage(kMsgTune);
	} else {
		fTuner = new UsbTuner();
		SetStatusText("no tuner opened - press U for the USB report");
		fVideoView->SetPlaceholder("no tuner");
	}
}


MainWindow::~MainWindow()
{
	delete fPlayer;
	delete fTuner;
}


void
MainWindow::BuildLayout()
{
	fChannelList = new BListView("channels", B_SINGLE_SELECTION_LIST);
	fChannelList->SetInvocationMessage(new BMessage(kMsgTune));
	for (size_t i = 0; i < fChannels.size(); i++)
		fChannelList->AddItem(new BStringItem(fChannels[i].Label().c_str()));
	fChannelList->Select(0);

	BScrollView* scroller = new BScrollView("channel-scroll", fChannelList,
		0, false, true);
	// Without a minimum of its own the list loses every argument with the
	// video view, which has one, and gets squeezed to nothing. Wide enough
	// for a service name plus its UHF number - "テスト放送  (UHF 13)" - since
	// a channel list you cannot read is not a channel list.
	scroller->SetExplicitMinSize(BSize(220, 120));

	// The scan button does the same thing as Alt-S, for a machine whose
	// pointing stick is easier to reach than its keyboard corner.
	fScanButton = new BButton("scan", "チャンネルスキャン", new BMessage(kMsgScan));

	fVideoView = new VideoView();
	fVideoView->SetExplicitMinSize(BSize(320, 240));

	fStatusView = new BStringView("status", "");

	BuildMenu();

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(fMenuBar)
		.AddGroup(B_HORIZONTAL, 0)
			.AddGroup(B_VERTICAL, B_USE_SMALL_SPACING, 0.28f)
				.Add(scroller)
				.Add(fScanButton)
			.End()
			.Add(fVideoView, 0.72f)
		.End()
		.Add(fStatusView)
	.End();

	fChannelList->MakeFocus(true);

	// Arrows move the selection and Enter tunes it, both straight out of
	// BListView - deliberately not tuning on every arrow press, because a
	// retune restarts the demodulator and holding Down would otherwise
	// thrash it. The rest are Command shortcuts; this machine's pointing
	// stick is bad enough that reaching for it should never be required.
	AddShortcut('U', B_COMMAND_KEY, new BMessage(kMsgUsbReport));
	AddShortcut('F', B_COMMAND_KEY, new BMessage(kMsgToggleScale));
	AddShortcut('.', B_COMMAND_KEY, new BMessage(kMsgStop));
	// Scan: walk every channel and mark the ones a stream actually comes out
	// of, then play the first. This is the "point it and go" path.
	AddShortcut('S', B_COMMAND_KEY, new BMessage(kMsgScan));
}


void
MainWindow::BuildMenu()
{
	// A single "ファイル" (File) menu: scan, settings, quit - in Japanese, since
	// this receives Japanese broadcast and its users read the channel names.
	fMenuBar = new BMenuBar("menubar");
	BMenu* file = new BMenu("ファイル");
	file->AddItem(new BMenuItem("スキャン", new BMessage(kMsgScan), 'S'));
	file->AddItem(new BMenuItem("設定" B_UTF8_ELLIPSIS,
		new BMessage(kMsgSettings)));
	file->AddSeparatorItem();
	file->AddItem(new BMenuItem("終了", new BMessage(kMsgQuit), 'Q'));
	fMenuBar->AddItem(file);
}


void
MainWindow::SetStatusText(const std::string& text)
{
	if (fStatusView != NULL)
		fStatusView->SetText(text.c_str());
}


void
MainWindow::TuneToSelection()
{
	int32 selected = fChannelList->CurrentSelection();
	if (selected < 0 || fTuner == NULL)
		return;

	const ChannelTable::Channel& channel = fChannels[selected];

	fPlayer->Stop();

	status_t status = fTuner->Open();
	if (status != B_OK) {
		std::string detail = fTuner->LastError();
		SetStatusText(detail.empty() ? "could not open the tuner" : detail);
		fVideoView->SetPlaceholder("no tuner");
		return;
	}

	status = fTuner->Tune(channel.frequencyHz);
	if (status != B_OK) {
		SetStatusText("could not tune " + channel.Label());
		return;
	}

	fVideoView->SetPlaceholder("tuning " + channel.Label() + "...");
	fPlayer->Start(fTuner);
}


void
MainWindow::StartScan()
{
	if (fScanThread >= 0)
		return;					// already scanning

	// Scanning drives the tuner directly, so nothing else may be reading it.
	fPlayer->Stop();

	UsbTuner* usb = dynamic_cast<UsbTuner*>(fTuner);
	if (usb == NULL) {
		SetStatusText("scan needs the USB tuner (a capture is already one "
			"channel)");
		return;
	}

	status_t status = fTuner->Open();
	if (status != B_OK) {
		std::string detail = fTuner->LastError();
		SetStatusText(detail.empty() ? "could not open the tuner" : detail);
		return;
	}

	// Reset every label to its bare UHF number so a re-scan starts clean.
	for (size_t i = 0; i < fChannels.size(); i++) {
		BStringItem* item = dynamic_cast<BStringItem*>(fChannelList->ItemAt(i));
		if (item != NULL)
			item->SetText(fChannels[i].Label().c_str());
	}
	fChannelList->Invalidate();

	fScanCancel = false;
	fScanFirstHit = -1;
	SetStatusText("scanning...");
	fScanThread = spawn_thread(ScanEntry, "roneseg scan", B_LOW_PRIORITY, this);
	if (fScanThread < 0) {
		fScanThread = -1;
		SetStatusText("could not start the scan");
		return;
	}
	resume_thread(fScanThread);

	if (fScanButton != NULL) {
		fScanButton->SetEnabled(false);
		fScanButton->SetLabel("スキャン中...");
	}
}


status_t
MainWindow::ScanEntry(void* self)
{
	MainWindow* window = (MainWindow*)self;
	UsbTuner* usb = dynamic_cast<UsbTuner*>(window->fTuner);
	if (usb == NULL)
		return B_ERROR;

	BMessenger messenger(window);
	for (size_t i = 0; i < window->fChannels.size(); i++) {
		if (window->fScanCancel)
			break;
		bool signal = usb->HasSignal(window->fChannels[i].frequencyHz);
		UsbTuner::Diagnostic diag = usb->LastDiagnostic();

		BMessage hit(kMsgScanHit);
		hit.AddInt32("index", (int32)i);
		hit.AddBool("signal", signal);
		hit.AddBool("tuned", diag.tuned);
		hit.AddBool("sync", diag.sync);
		hit.AddInt64("bytes", (int64)diag.bytes);
		messenger.SendMessage(&hit);
	}
	messenger.SendMessage(new BMessage(kMsgScanDone));
	return B_OK;
}


void
MainWindow::ShowUsbReport()
{
	std::string report = UsbTuner::ScanReport();
	BAlert* alert = new BAlert("USB devices", report.c_str(), "OK");
	alert->SetShortcut(0, B_ESCAPE);
	alert->Go(NULL);
}


void
MainWindow::ShowSettings()
{
	UsbTuner* usb = dynamic_cast<UsbTuner*>(fTuner);
	if (usb == NULL) {
		SetStatusText("設定はUSBチューナー使用時のみ");
		return;
	}

	// A tiny modal panel with one field: the demodulator register the frequency
	// word is written to. The default 0x32 came from static analysis and could
	// not be confirmed against a live signal, so it is adjustable here rather
	// than by recompiling - the one knob that might need turning in Japan.
	char current[8];
	snprintf(current, sizeof(current), "%02x", usb->FrequencyRegister());

	BWindow* panel = new BWindow(
		BRect(0, 0, 320, 110), "設定", B_MODAL_WINDOW,
		B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS);
	panel->CenterIn(Frame());

	BTextControl* field = new BTextControl("freqreg",
		"周波数レジスタ (16進):", current, NULL);

	BButton* ok = new BButton("ok", "OK", new BMessage(kMsgApplySettings));
	BButton* cancel = new BButton("cancel", "キャンセル",
		new BMessage(B_QUIT_REQUESTED));

	BLayoutBuilder::Group<>(panel, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_INSETS)
		.Add(field)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.AddGlue()
			.Add(cancel)
			.Add(ok)
		.End()
	.End();

	// The buttons target this window, so OK's message carries the field's text
	// back here. Wire OK and the field to send to the main window.
	ok->SetTarget(this);
	field->SetTarget(this);
	// Stash the field's text on the apply message by making Enter in the field
	// also apply.
	field->SetModificationMessage(NULL);

	// Remember the panel and field so ApplySettings can read and close them.
	fSettingsPanel = panel;
	fSettingsField = field;
	cancel->SetTarget(panel);

	panel->Show();
	field->MakeFocus(true);
}


void
MainWindow::ApplySettings(BMessage* message)
{
	(void)message;
	UsbTuner* usb = dynamic_cast<UsbTuner*>(fTuner);
	if (usb != NULL && fSettingsField != NULL) {
		const char* text = fSettingsField->Text();
		if (text != NULL && text[0] != '\0') {
			long reg = strtol(text, NULL, 16);
			if (reg >= 0 && reg <= 0xFE) {
				usb->SetFrequencyRegister((uint8)reg);
				char status[96];
				snprintf(status, sizeof(status),
					"周波数レジスタを 0x%02lx に設定 - 再スキャンしてください",
					reg);
				SetStatusText(status);
			}
		}
	}
	if (fSettingsPanel != NULL) {
		fSettingsPanel->PostMessage(B_QUIT_REQUESTED);
		fSettingsPanel = NULL;
		fSettingsField = NULL;
	}
}


void
MainWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgTune:
			TuneToSelection();
			break;

		case kMsgStop:
			fPlayer->Stop();
			SetStatusText("stopped");
			break;

		case kMsgUsbReport:
			ShowUsbReport();
			break;

		case kMsgScan:
			StartScan();
			break;

		case kMsgSettings:
			ShowSettings();
			break;

		case kMsgApplySettings:
			ApplySettings(message);
			break;

		case kMsgQuit:
			PostMessage(B_QUIT_REQUESTED);
			break;

		case kMsgScanHit:
		{
			int32 index = -1;
			if (message->FindInt32("index", &index) != B_OK)
				break;

			bool signal = false, tuned = false, sync = false;
			int64 bytes = 0;
			message->FindBool("signal", &signal);
			message->FindBool("tuned", &tuned);
			message->FindBool("sync", &sync);
			message->FindInt64("bytes", &bytes);

			int physical = fChannels[index].physical;

			// Diagnostic line for every channel, so a failed scan in the field
			// can be told apart: no bytes at all means nothing tuned or the
			// stream never started; bytes without sync means data arrives but
			// does not frame as TS (wrong frequency register, most likely);
			// bytes with sync is a real channel.
			BString log;
			log << "UHF " << physical << ": ";
			if (!tuned)
				log << "tune failed";
			else if (bytes <= 0)
				log << "no data (0 bytes) - not locked or stream not started";
			else if (!sync)
				log << bytes << " bytes, no TS sync - check freq register (設定)";
			else
				log << bytes << " bytes, TS sync - receiving";
			SetStatusText(std::string(log.String()));

			if (signal) {
				BStringItem* item
					= dynamic_cast<BStringItem*>(fChannelList->ItemAt(index));
				if (item != NULL) {
					BString label("* ");
					label << item->Text();
					item->SetText(label.String());
					fChannelList->InvalidateItem(index);
				}
				if (fScanFirstHit < 0)
					fScanFirstHit = index;
			}
			break;
		}

		case kMsgScanDone:
		{
			fScanThread = -1;
			if (fScanButton != NULL) {
				fScanButton->SetEnabled(true);
				fScanButton->SetLabel("チャンネルスキャン");
			}
			// If a quit was deferred until the scan stopped, do it now.
			if (fQuitPending) {
				PostMessage(B_QUIT_REQUESTED);
				break;
			}
			if (fScanFirstHit >= 0) {
				fChannelList->Select(fScanFirstHit);
				fChannelList->ScrollToSelection();
				SetStatusText("scan done - playing the first channel");
				PostMessage(kMsgTune);
			} else {
				SetStatusText("scan done - no channel is receiving here");
			}
			break;
		}

		case kMsgToggleScale:
			fVideoView->SetScaled(!fVideoView->IsScaled());
			SetStatusText(fVideoView->IsScaled()
				? "scaled to fit - costs CPU this machine does not have"
				: "1:1");
			break;

		case TunerAdapterIO::kServiceNameMessage:
		{
			BString name;
			if (message->FindString("name", &name) != B_OK || name.Length() == 0)
				break;

			// Relabel the tuned entry in place. The channel list starts out
			// showing UHF numbers because that is all a receiver knows before
			// it has decoded anything; the name replaces it once the SDT
			// arrives, and the number stays alongside it because that is what
			// you retune by when a scan goes wrong.
			int32 selected = fChannelList->CurrentSelection();
			if (selected < 0)
				break;
			BStringItem* item
				= dynamic_cast<BStringItem*>(fChannelList->ItemAt(selected));
			if (item == NULL)
				break;

			BString label;
			label << name << "  (UHF " << fChannels[selected].physical << ")";
			item->SetText(label.String());
			fChannelList->InvalidateItem(selected);
			break;
		}

		case Player::kStatusMessage:
		{
			int32 state = 0;
			BString detail;
			message->FindInt32("state", &state);
			message->FindString("detail", &detail);

			switch (state) {
				case Player::kTuning:
					SetStatusText("tuning...");
					break;
				case Player::kPlaying:
					SetStatusText(std::string("playing - ") + detail.String());
					break;
				case Player::kStopped:
					SetStatusText("stopped");
					break;
				case Player::kError:
					SetStatusText(std::string("error: ") + detail.String());
					fVideoView->SetPlaceholder("no signal");
					break;
			}
			break;
		}

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


bool
MainWindow::QuitRequested()
{
	// A scan runs on its own thread and holds the tuner. Blocking here to wait
	// for it made quit appear to hang - a channel with no signal takes a moment
	// to time out, and the window thread could not do anything meanwhile. So
	// just ask the scan to stop and defer the quit: when it posts kMsgScanDone,
	// that handler quits for us. The window stays responsive in between.
	if (fScanThread >= 0) {
		fScanCancel = true;
		fQuitPending = true;
		SetStatusText("stopping scan...");
		return false;
	}
	fPlayer->Stop();
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}
