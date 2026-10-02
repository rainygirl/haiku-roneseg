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

#include <stdlib.h>
#include <ListItem.h>
#include <ListView.h>
#include <ScrollView.h>
#include <StringView.h>

#include <new>

#include "DiagnosticWindow.h"
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
static const uint32 kMsgQuit = 'Quit';
static const uint32 kMsgDiagnose = 'Diag';
static const uint32 kMsgMeter = 'Metr';
static const uint32 kMsgMeterNote = 'MtrN';
static const uint32 kMsgMeterDone = 'MtrD';


// "+1.3 dB  ||||" - one bar per dB, so a channel that is coming in is
// obvious at a glance.
static BString
StrengthText(float dB)
{
	BString text;
	text.SetToFormat("%+.1f dB  ", dB);
	int bars = (int)(dB + 0.5f);
	if (bars > 30)
		bars = 30;
	for (int i = 0; i < bars; i++)
		text << "|";
	return text;
}


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
	fPlayer(NULL),
	fTuner(NULL),
	fScanThread(-1),
	fScanCancel(false),
	fQuitPending(false),
	fScanFirstHit(-1),
	fMeterRunning(false),
	fMeterChannel(-1),
	fDiagnosticTuner(NULL)
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
	CloseDiagnostic();
	delete fPlayer;
	delete fTuner;
	delete fDiagnosticTuner;
}


// Synchronous: the diagnostic thread uses a tuner this window owns, so the
// diagnostic window has to be gone before either is deleted.
void
MainWindow::CloseDiagnostic()
{
	if (!fDiagnostic.IsValid())
		return;
	BMessage reply;
	fDiagnostic.SendMessage(B_QUIT_REQUESTED, &reply);
	fDiagnostic = BMessenger();
}


bool
MainWindow::DiagnosticBusy()
{
	if (!fDiagnostic.IsValid())
		return false;
	SetStatusText("チューナー診断中 - 診断ウィンドウを閉じてください");
	return true;
}


void
MainWindow::ShowDiagnostic()
{
	if (fDiagnostic.IsValid()) {
		BLooper* looper = NULL;
		fDiagnostic.Target(&looper);
		BWindow* window = dynamic_cast<BWindow*>(looper);
		if (window != NULL && window->Lock()) {
			window->Activate();
			window->Unlock();
		}
		return;
	}
	if (fScanThread >= 0) {
		SetStatusText("スキャン中は診断できません");
		return;
	}

	// The diagnostic drives the module directly, so playback must stop.
	fPlayer->Stop();

	UsbTuner* usb = dynamic_cast<UsbTuner*>(fTuner);
	if (usb == NULL) {
		// Replaying a capture: the internal tuner is not otherwise in use,
		// so the diagnostic gets an instance of its own.
		if (fDiagnosticTuner == NULL) {
			fDiagnosticTuner = new(std::nothrow) UsbTuner();
			if (fDiagnosticTuner == NULL)
				return;
		}
		usb = fDiagnosticTuner;
	}

	BRect frame(0, 0, 640, 480);
	frame.OffsetTo(Frame().left + 40, Frame().top + 40);
	DiagnosticWindow* window = new(std::nothrow) DiagnosticWindow(frame, usb);
	if (window == NULL)
		return;
	fDiagnostic = BMessenger(window);
	window->Show();
	SetStatusText("チューナー診断を実行中...");
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
	// A BStringView's maximum width is its text width, and with
	// B_AUTO_UPDATE_SIZE_LIMITS that became the window's maximum width: the
	// window opened narrower than asked and could not be widened at all.
	fStatusView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

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
	// Retunes the selected channel over and over and shows how strong it is,
	// for finding a place (or an antenna position) where it comes in.
	file->AddItem(new BMenuItem("信号メーター", new BMessage(kMsgMeter), 'M'));
	// Checks the internal module stage by stage without needing a broadcast:
	// USB, firmware, register bus, demodulator, then every channel's lock
	// and signal strength.
	file->AddItem(new BMenuItem("チューナー診断" B_UTF8_ELLIPSIS,
		new BMessage(kMsgDiagnose)));
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

	if (DiagnosticBusy())
		return;
	if (fScanThread >= 0)
		return;					// the scan or the meter holds the tuner

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

	// Only start reading once the demodulator has locked: a bulk read on a
	// channel that is not there never completes and wedges the module.
	UsbTuner* usb = dynamic_cast<UsbTuner*>(fTuner);
	if (usb != NULL && usb->WaitForLock(1500000) != UsbTuner::kLocked) {
		BString log;
		log << "UHF " << channel.physical << ": ロックなし";
		float strength = 0;
		if (usb->MeasureSignal(&strength))
			log << "  強度 " << StrengthText(strength);
		SetStatusText(std::string(log.String()));
		fVideoView->SetPlaceholder("no signal");
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
	if (DiagnosticBusy())
		return;

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
		hit.AddBool("locked", diag.lock == UsbTuner::kLocked);
		if (diag.measured)
			hit.AddFloat("strength", diag.strength);
		messenger.SendMessage(&hit);
	}
	messenger.SendMessage(new BMessage(kMsgScanDone));
	return B_OK;
}


void
MainWindow::ToggleMeter()
{
	if (fScanThread >= 0) {
		// Choosing it again stops it; a running scan is left alone.
		if (fMeterRunning)
			fScanCancel = true;
		return;
	}
	if (DiagnosticBusy())
		return;

	fPlayer->Stop();

	UsbTuner* usb = dynamic_cast<UsbTuner*>(fTuner);
	int32 selected = fChannelList->CurrentSelection();
	if (usb == NULL || selected < 0) {
		SetStatusText("信号メーターはUSBチューナーでチャンネルを選んでから");
		return;
	}

	status_t status = fTuner->Open();
	if (status != B_OK) {
		std::string detail = fTuner->LastError();
		SetStatusText(detail.empty() ? "could not open the tuner" : detail);
		return;
	}

	fMeterChannel = selected;
	fScanCancel = false;
	fMeterRunning = true;
	SetStatusText("信号メーター - " + fChannels[selected].Label());
	fScanThread = spawn_thread(MeterEntry, "roneseg meter", B_LOW_PRIORITY,
		this);
	if (fScanThread < 0) {
		fScanThread = -1;
		fMeterRunning = false;
		SetStatusText("could not start the meter");
		return;
	}
	resume_thread(fScanThread);

	if (fScanButton != NULL) {
		fScanButton->SetEnabled(false);
		fScanButton->SetLabel("信号メーター中...");
	}
}


// Retunes the one channel until it locks or the meter is stopped: the
// demodulator gives up on a channel it cannot find and has to be asked again,
// and each attempt is also a fresh strength reading.
status_t
MainWindow::MeterEntry(void* self)
{
	MainWindow* window = (MainWindow*)self;
	UsbTuner* usb = dynamic_cast<UsbTuner*>(window->fTuner);
	if (usb == NULL)
		return B_ERROR;

	uint64 frequency = window->fChannels[window->fMeterChannel].frequencyHz;
	BMessenger messenger(window);
	bool received = false;
	while (!window->fScanCancel && !received) {
		received = usb->HasSignal(frequency);
		UsbTuner::Diagnostic diag = usb->LastDiagnostic();

		BMessage note(kMsgMeterNote);
		note.AddBool("locked", diag.lock == UsbTuner::kLocked);
		note.AddBool("received", received);
		if (diag.measured)
			note.AddFloat("strength", diag.strength);
		messenger.SendMessage(&note);
		if (!diag.tuned)
			snooze(500000);
	}

	BMessage done(kMsgMeterDone);
	done.AddBool("received", received);
	messenger.SendMessage(&done);
	return B_OK;
}


void
MainWindow::FinishScanUi()
{
	fScanThread = -1;
	fMeterRunning = false;
	if (fScanButton != NULL) {
		fScanButton->SetEnabled(true);
		fScanButton->SetLabel("チャンネルスキャン");
	}
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
MainWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgTune:
			TuneToSelection();
			break;

		case kMsgStop:
			if (fMeterRunning) {
				fScanCancel = true;
				break;
			}
			fPlayer->Stop();
			SetStatusText("stopped");
			break;

		case kMsgUsbReport:
			ShowUsbReport();
			break;

		case kMsgScan:
			StartScan();
			break;

		case kMsgMeter:
			ToggleMeter();
			break;

		case kMsgMeterNote:
		{
			bool locked = false, received = false;
			float strength = 0;
			message->FindBool("locked", &locked);
			message->FindBool("received", &received);
			bool measured = message->FindFloat("strength", &strength) == B_OK;

			BString log;
			log << "UHF " << fChannels[fMeterChannel].physical << "  ";
			if (measured)
				log << "強度 " << StrengthText(strength) << "  ";
			log << (received ? "受信" : locked ? "ロック" : "ロックなし");
			if (!received)
				log << "  (もう一度メニューで停止)";
			SetStatusText(std::string(log.String()));
			break;
		}

		case kMsgMeterDone:
		{
			bool received = false;
			message->FindBool("received", &received);
			FinishScanUi();
			if (fQuitPending) {
				PostMessage(B_QUIT_REQUESTED);
				break;
			}
			if (received) {
				fChannelList->Select(fMeterChannel);
				PostMessage(kMsgTune);
			} else
				SetStatusText("信号メーター停止");
			break;
		}

		case kMsgDiagnose:
			ShowDiagnostic();
			break;

		case kMsgQuit:
			PostMessage(B_QUIT_REQUESTED);
			break;

		case kMsgScanHit:
		{
			int32 index = -1;
			if (message->FindInt32("index", &index) != B_OK)
				break;

			bool signal = false, tuned = false, sync = false, locked = false;
			int64 bytes = 0;
			float strength = 0;
			message->FindBool("signal", &signal);
			message->FindBool("tuned", &tuned);
			message->FindBool("sync", &sync);
			message->FindBool("locked", &locked);
			message->FindInt64("bytes", &bytes);
			bool measured = message->FindFloat("strength", &strength) == B_OK;

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
			else if (!locked)
				log << "ロックなし";
			else if (bytes <= 0)
				log << "ロックしたがデータなし";
			else if (!sync)
				log << bytes << " bytes, no TS sync";
			else
				log << bytes << " bytes, TS sync - receiving";
			if (measured)
				log << "  強度 " << StrengthText(strength);
			SetStatusText(std::string(log.String()));

			// Every scanned channel keeps its strength in the list, so the
			// whole band can be read off after the scan.
			if (measured) {
				BStringItem* item
					= dynamic_cast<BStringItem*>(fChannelList->ItemAt(index));
				if (item != NULL) {
					BString label(fChannels[index].Label().c_str());
					label << "  " << StrengthText(strength);
					item->SetText(label.String());
					fChannelList->InvalidateItem(index);
				}
			}

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
			FinishScanUi();
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
				? "fit to window"
				: "1:1 - original size, least CPU");
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
	CloseDiagnostic();
	fPlayer->Stop();
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}
