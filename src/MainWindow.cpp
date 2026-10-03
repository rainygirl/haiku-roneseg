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
#include <Screen.h>
#include <Slider.h>
#include <GroupView.h>

#include <stdlib.h>
#include <ListItem.h>
#include <ListView.h>
#include <ScrollView.h>
#include <StringView.h>

#include <new>

#include "DiagnosticWindow.h"
#include "Localize.h"
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
static const uint32 kMsgFullscreen = 'Full';
static const uint32 kMsgExitFullscreen = 'ExFu';
static const uint32 kMsgVolume = 'Volm';
static const uint32 kMsgTuneDone = 'Tdon';
static const uint32 kMsgPrepare = 'Prep';

class ChannelList : public BListView {
public:
	ChannelList() : BListView("channels", B_SINGLE_SELECTION_LIST) {}
	void MouseDown(BPoint where)
	{
		int32 index = IndexOf(where);
		BListView::MouseDown(where);
		// Single click tunes; keyboard arrows remain free to browse.
		if (index >= 0) Invoke();
	}
};


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
	fVolumeSlider(NULL),
	fSidebar(NULL),
	fControls(NULL),
	fFullscreen(false),
	fWindowedScaled(true),
	fPlayer(NULL),
	fTuner(NULL),
	fScanThread(-1),
	fTuneThread(-1),
	fTuneCancel(0),
	fTuneIndex(-1),
	fPendingTune(-1),
	fScanCancel(false),
	fQuitPending(false),
	fScanFirstHit(-1),
	fScanStartIndex(0),
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
		SetStatusText("preparing receiver...");
		fVideoView->SetPlaceholder("preparing receiver...");
		PostMessage(kMsgPrepare);
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
	if (fTuneThread >= 0) {
		SetStatusText(Tr("受信を準備中...", "Preparing reception..."));
		return true;
	}
	if (!fDiagnostic.IsValid())
		return false;
	SetStatusText(Tr("チューナー診断中 - 診断ウィンドウを閉じてください",
		"Tuner diagnostic running - close its window first"));
	return true;
}


void
MainWindow::ShowDiagnostic()
{
	if (fTuneThread >= 0) {
		SetStatusText("Stop reception preparation before opening the diagnostic");
		return;
	}
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
		SetStatusText(Tr("スキャン中は診断できません", "Cannot diagnose while scanning"));
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
	SetStatusText(Tr("チューナー診断を実行中...", "Running the tuner diagnostic..."));
}


void
MainWindow::BuildLayout()
{
	fChannelList = new ChannelList();
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
	fScanButton = new BButton("scan", Tr("チャンネルスキャン", "Channel scan"),
		new BMessage(kMsgScan));

	fVideoView = new VideoView();
	fVideoView->SetExplicitMinSize(BSize(320, 240));

	fStatusView = new BStringView("status", "");
	// A BStringView's maximum width is its text width, and with
	// B_AUTO_UPDATE_SIZE_LIMITS that became the window's maximum width: the
	// window opened narrower than asked and could not be widened at all.
	fStatusView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	fVolumeSlider = new BSlider("volume", Tr("音量", "Volume"),
		new BMessage(kMsgVolume), 0, 100, B_HORIZONTAL);
	fVolumeSlider->SetValue(100);
	fVolumeSlider->SetModificationMessage(new BMessage(kMsgVolume));
	fVolumeSlider->SetExplicitMinSize(BSize(140, B_SIZE_UNSET));
	fVolumeSlider->SetExplicitMaxSize(BSize(220, B_SIZE_UNSET));
	BButton* fullscreen = new BButton("fullscreen",
		Tr("全画面", "Fullscreen"), new BMessage(kMsgFullscreen));
	fSidebar = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	fControls = new BGroupView(B_HORIZONTAL, B_USE_SMALL_SPACING);
	BLayoutBuilder::Group<>((BGroupView*)fSidebar)
		.Add(scroller).Add(fScanButton);
	BLayoutBuilder::Group<>((BGroupView*)fControls)
		.SetInsets(B_USE_SMALL_INSETS)
		.Add(fStatusView).Add(fVolumeSlider).Add(fullscreen);

	BuildMenu();

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(fMenuBar)
		.AddGroup(B_HORIZONTAL, 0)
			.Add(fSidebar, 0.28f)
			.Add(fVideoView, 0.72f)
		.End()
		.Add(fControls)
	.End();

	fChannelList->MakeFocus(true);

	// Arrows move the selection and Enter tunes it, both straight out of
	// BListView - deliberately not tuning on every arrow press, because a
	// retune restarts the demodulator and holding Down would otherwise
	// thrash it. The rest are Command shortcuts; this machine's pointing
	// stick is bad enough that reaching for it should never be required.
	AddShortcut('U', B_COMMAND_KEY, new BMessage(kMsgUsbReport));
	AddShortcut('F', B_COMMAND_KEY, new BMessage(kMsgToggleScale));
	AddShortcut('F', B_COMMAND_KEY | B_SHIFT_KEY, new BMessage(kMsgFullscreen));
	AddShortcut(B_ESCAPE, 0, new BMessage(kMsgExitFullscreen));
	AddShortcut('.', B_COMMAND_KEY, new BMessage(kMsgStop));
	// Scan: walk every channel and mark the ones a stream actually comes out
	// of, then play the first. This is the "point it and go" path.
	AddShortcut('S', B_COMMAND_KEY, new BMessage(kMsgScan));
}


void
MainWindow::BuildMenu()
{
	// A single File menu: scan, meter, diagnostic, quit. Japanese when the
	// system language is Japanese, English otherwise (Localize.h).
	fMenuBar = new BMenuBar("menubar");
	BMenu* file = new BMenu(Tr("ファイル", "File"));
	file->AddItem(new BMenuItem(Tr("スキャン", "Scan"), new BMessage(kMsgScan),
		'S'));
	// Retunes the selected channel over and over and shows how strong it is,
	// for finding a place (or an antenna position) where it comes in.
	file->AddItem(new BMenuItem(Tr("信号メーター", "Signal meter"),
		new BMessage(kMsgMeter), 'M'));
	// Checks the internal module stage by stage without needing a broadcast:
	// USB, firmware, register bus, demodulator, then every channel's lock
	// and signal strength.
	BString diagnose(Tr("チューナー診断", "Tuner diagnostic"));
	diagnose << B_UTF8_ELLIPSIS;
	file->AddItem(new BMenuItem(diagnose.String(),
		new BMessage(kMsgDiagnose)));
	file->AddSeparatorItem();
	file->AddItem(new BMenuItem(Tr("終了", "Quit"), new BMessage(kMsgQuit), 'Q'));
	fMenuBar->AddItem(file);
	BMenu* view = new BMenu(Tr("表示", "View"));
	view->AddItem(new BMenuItem(Tr("全画面", "Fullscreen"),
		new BMessage(kMsgFullscreen), 'F', B_COMMAND_KEY | B_SHIFT_KEY));
	view->AddItem(new BMenuItem(Tr("画面に合わせる / 原寸", "Fit / original size"),
		new BMessage(kMsgToggleScale), 'F'));
	fMenuBar->AddItem(view);
}


void
MainWindow::ToggleFullscreen()
{
	if (!fFullscreen) {
		fWindowedFrame = Frame();
		fWindowedScaled = fVideoView->IsScaled();
		fFullscreen = true;
		fSidebar->Hide();
		fControls->Hide();
		fMenuBar->Hide();
		SetLook(B_NO_BORDER_WINDOW_LOOK);
		fVideoView->SetScaled(true);
		BRect screen = BScreen(this).Frame();
		MoveTo(screen.LeftTop());
		ResizeTo(screen.Width(), screen.Height());
	} else {
		fFullscreen = false;
		SetLook(B_TITLED_WINDOW_LOOK);
		fSidebar->Show();
		fControls->Show();
		fMenuBar->Show();
		fVideoView->SetScaled(fWindowedScaled);
		MoveTo(fWindowedFrame.LeftTop());
		ResizeTo(fWindowedFrame.Width(), fWindowedFrame.Height());
	}
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
	{
		fScanCancel = true;
		fPendingTune = selected;
		SetStatusText("stopping scan to play the selected channel...");
		return;
	}

	StartTuning(selected);
}


void
MainWindow::StartTuning(int32 selected)
{
	fPlayer->Stop();
	fTuneIndex = selected;
	atomic_set(&fTuneCancel, 0);
	UsbTuner* usb = dynamic_cast<UsbTuner*>(fTuner);
	if (usb) usb->BeginPlayback();
	SetStatusText(Tr("受信を準備中...", "Preparing reception on VAIO..."));
	fVideoView->SetPlaceholder("preparing reception...");
	fTuneThread = spawn_thread(TuneEntry, "oneseg tune", B_NORMAL_PRIORITY, this);
	if (fTuneThread < 0) {
		SetStatusText("could not start tuning");
		return;
	}
	fScanButton->SetEnabled(false);
	resume_thread(fTuneThread);
}


status_t
MainWindow::TuneEntry(void* cookie)
{
	MainWindow* window = (MainWindow*)cookie;
	Tuner* tuner = window->fTuner;
	UsbTuner* usb = dynamic_cast<UsbTuner*>(tuner);
	status_t status = tuner->Open();
	std::string error;
	if (status == B_OK && usb && !atomic_get(&window->fTuneCancel))
		status = usb->PreparePlayback();
	if (status == B_OK && window->fTuneIndex >= 0 && !atomic_get(&window->fTuneCancel))
		status = tuner->Tune(window->fChannels[window->fTuneIndex].frequencyHz);
	if (status == B_OK && usb && window->fTuneIndex >= 0 && !atomic_get(&window->fTuneCancel)) {
		if (usb->WaitForLock(6000000) != UsbTuner::kLocked) {
			status = B_ERROR;
			error = "No signal - check the antenna and select the channel again";
		} else if (!atomic_get(&window->fTuneCancel))
			status = usb->StartReading();
	}
	if (atomic_get(&window->fTuneCancel)) status = B_CANCELED;
	BMessage done(kMsgTuneDone);
	done.AddInt32("status", status);
	done.AddString("error", error.empty() ? tuner->LastError().c_str() : error.c_str());
	BMessenger(window).SendMessage(&done);
	return B_OK;
}


void
MainWindow::StartScan()
{
	if (fScanThread >= 0) {
		fScanCancel = true;
		SetStatusText("stopping scan...");
		return;
	}
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
	fScanStartIndex = fChannelList->CurrentSelection();
	if (fScanStartIndex < 0) fScanStartIndex = 0;
	SetStatusText("scanning...");
	fScanThread = spawn_thread(ScanEntry, "roneseg scan", B_LOW_PRIORITY, this);
	if (fScanThread < 0) {
		fScanThread = -1;
		SetStatusText("could not start the scan");
		return;
	}
	resume_thread(fScanThread);

	if (fScanButton != NULL) {
		fScanButton->SetEnabled(true);
		fScanButton->SetLabel(Tr("スキャン停止", "Stop scan"));
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
	// Start where the user is looking, then wrap to cover the full band.
	for (size_t offset = 0; offset < window->fChannels.size(); offset++) {
		if (window->fScanCancel)
			break;
		size_t i = (window->fScanStartIndex + offset) % window->fChannels.size();
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
		SetStatusText(Tr("信号メーターはUSBチューナーでチャンネルを選んでから",
			"Select a channel on the USB tuner to use the signal meter"));
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
	SetStatusText(Tr("信号メーター - ", "Signal meter - ") + fChannels[selected].Label());
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
		fScanButton->SetLabel(Tr("信号メーター中...", "Metering..."));
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
		fScanButton->SetLabel(Tr("チャンネルスキャン", "Channel scan"));
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
		case TunerAdapterIO::kStreamErrorMessage:
		{
			int64 generation;
			const char* detail;
			if (message->FindInt64("generation", &generation) == B_OK
				&& message->FindString("detail", &detail) == B_OK
				&& fPlayer->IsCurrentGeneration(generation)) {
				fPlayer->Stop(false);
				SetStatusText(detail);
				fVideoView->SetPlaceholder(detail);
			}
			break;
		}
		case kMsgPrepare:
			if (fTuneThread < 0 && fScanThread < 0 && !DiagnosticBusy())
				StartTuning(-1);
			break;
		case kMsgTuneDone:
		{
			status_t exitStatus;
			if (fTuneThread >= 0) wait_for_thread(fTuneThread, &exitStatus);
			fTuneThread = -1;
			fScanButton->SetEnabled(true);
			int32 status = B_ERROR;
			message->FindInt32("status", &status);
			if (fQuitPending) {
				PostMessage(B_QUIT_REQUESTED);
				break;
			}
			if (status == B_OK && !atomic_get(&fTuneCancel)) {
				if (fTuneIndex >= 0) {
					fChannelList->Select(fTuneIndex);
					fPlayer->Start(fTuner);
				} else {
					SetStatusText("Ready - connect the antenna and scan for channels");
					fVideoView->SetPlaceholder("select a channel");
				}
			} else {
				const char* error = NULL;
				message->FindString("error", &error);
				std::string detail = status == B_CANCELED ? "stopped"
					: error && *error ? error : "could not start reception";
				SetStatusText(detail);
				fVideoView->SetPlaceholder(detail);
			}
			break;
		}
		case kMsgVolume:
			fPlayer->SetVolume(fVolumeSlider->Value() / 100.0f);
			break;
		case kMsgFullscreen:
			ToggleFullscreen();
			break;
		case kMsgExitFullscreen:
			if (fFullscreen)
				ToggleFullscreen();
			break;
		case kMsgTune:
			TuneToSelection();
			break;

		case kMsgStop:
			if (fTuneThread >= 0) {
				atomic_set(&fTuneCancel, 1);
				fTuner->CancelRead();
				SetStatusText("stopping...");
				break;
			}
			if (fScanThread >= 0) {
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
				log << Tr("強度 ", "strength ") << StrengthText(strength) << "  ";
			log << (received ? Tr("受信", "receiving")
				: locked ? Tr("ロック", "locked") : Tr("ロックなし", "no lock"));
			if (!received)
				log << Tr("  (もう一度メニューで停止)", "  (choose it again to stop)");
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
			if (fPendingTune >= 0) {
				fChannelList->Select(fPendingTune);
				fPendingTune = -1;
				PostMessage(kMsgTune);
				break;
			}
			if (received) {
				fChannelList->Select(fMeterChannel);
				PostMessage(kMsgTune);
			} else
				SetStatusText(Tr("信号メーター停止", "Signal meter stopped"));
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
				log << Tr("ロックなし", "no lock");
			else if (signal)
				log << Tr("受信可能 - 選択して再生", "signal found - select to play");
			else if (bytes <= 0)
				log << "locked";
			else if (!sync)
				log << bytes << " bytes, no TS sync";
			else
				log << bytes << " bytes, TS sync - receiving";
			if (measured)
				log << Tr("  強度 ", "  strength ") << StrengthText(strength);
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
			if (fPendingTune >= 0) {
				fChannelList->Select(fPendingTune);
				fPendingTune = -1;
				PostMessage(kMsgTune);
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
			int32 selected = fTuneIndex;
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
	if (fTuneThread >= 0) {
		fQuitPending = true;
		atomic_set(&fTuneCancel, 1);
		fTuner->CancelRead();
		SetStatusText("stopping...");
		return false;
	}
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
