#include "MainWindow.h"

#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <OS.h>
#include <RadioButton.h>
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
static const uint32 kMsgSweep = 'Swep';
static const uint32 kMsgSweepHit = 'SwHt';
static const uint32 kMsgSweepDone = 'SwDn';
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
	fSettingsLowField(NULL),
	fSettingsLatchField(NULL),
	fPresetDefault(NULL),
	fPresetAlternate(NULL),
	fPlayer(NULL),
	fTuner(NULL),
	fScanThread(-1),
	fScanCancel(false),
	fQuitPending(false),
	fScanFirstHit(-1),
	fSweepChannel(-1),
	fSweepSavedHigh(0),
	fSweepSavedLow(0),
	fSweepSavedLatch(0)
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
	// The sweep is the answer to "which registers?" - it tries every candidate
	// layout against the selected channel and keeps the one that receives.
	file->AddItem(new BMenuItem("周波数レジスタ候補スイープ",
		new BMessage(kMsgSweep)));
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
MainWindow::StartSweep()
{
	if (fScanThread >= 0)
		return;					// a scan or a sweep is already running

	fPlayer->Stop();

	UsbTuner* usb = dynamic_cast<UsbTuner*>(fTuner);
	if (usb == NULL) {
		SetStatusText("スイープはUSBチューナー使用時のみ");
		return;
	}

	int32 selected = fChannelList->CurrentSelection();
	if (selected < 0) {
		SetStatusText("放送のあるチャンネルを選んでからスイープしてください");
		return;
	}

	status_t status = fTuner->Open();
	if (status != B_OK) {
		std::string detail = fTuner->LastError();
		SetStatusText(detail.empty() ? "could not open the tuner" : detail);
		return;
	}

	// Remember the layout in use: a sweep that finds nothing must leave the
	// tuner exactly as it was, not on whatever candidate happened to be last.
	fSweepSavedHigh = usb->FrequencyRegister();
	fSweepSavedLow = usb->FrequencyRegisterLow();
	fSweepSavedLatch = usb->LatchValue();

	fSweepChannel = selected;
	fScanCancel = false;
	SetStatusText("候補スイープ中 - " + fChannels[selected].Label());
	fScanThread = spawn_thread(SweepEntry, "roneseg sweep", B_LOW_PRIORITY,
		this);
	if (fScanThread < 0) {
		fScanThread = -1;
		SetStatusText("could not start the sweep");
		return;
	}
	resume_thread(fScanThread);

	if (fScanButton != NULL) {
		fScanButton->SetEnabled(false);
		fScanButton->SetLabel("スイープ中...");
	}
}


status_t
MainWindow::SweepEntry(void* self)
{
	MainWindow* window = (MainWindow*)self;
	UsbTuner* usb = dynamic_cast<UsbTuner*>(window->fTuner);
	if (usb == NULL)
		return B_ERROR;

	uint64 frequency = window->fChannels[window->fSweepChannel].frequencyHz;
	size_t count = 0;
	const UsbTuner::TuningCandidate* candidates
		= UsbTuner::TuningCandidates(&count);

	BMessenger messenger(window);
	int32 hit = -1;
	for (size_t i = 0; i < count; i++) {
		if (window->fScanCancel)
			break;

		usb->SetFrequencyRegisters(candidates[i].high, candidates[i].low);
		usb->SetLatchValue(candidates[i].latch);

		bool signal = usb->HasSignal(frequency);
		UsbTuner::Diagnostic diag = usb->LastDiagnostic();

		BMessage note(kMsgSweepHit);
		note.AddInt32("index", (int32)i);
		note.AddInt32("count", (int32)count);
		note.AddInt32("high", candidates[i].high);
		note.AddInt32("low", candidates[i].low);
		note.AddInt32("latch", candidates[i].latch);
		note.AddBool("signal", signal);
		note.AddBool("sync", diag.sync);
		note.AddInt64("bytes", (int64)diag.bytes);
		messenger.SendMessage(&note);

		if (signal) {
			hit = (int32)i;
			break;				// the tuner is already set to this layout
		}
	}

	BMessage done(kMsgSweepDone);
	done.AddInt32("hit", hit);
	messenger.SendMessage(&done);
	return B_OK;
}


void
MainWindow::FinishScanUi()
{
	fScanThread = -1;
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
MainWindow::ShowSettings()
{
	UsbTuner* usb = dynamic_cast<UsbTuner*>(fTuner);
	if (usb == NULL) {
		SetStatusText("設定はUSBチューナー使用時のみ");
		return;
	}

	// A tiny modal panel for the one thing static analysis could not settle:
	// where the frequency word lands. Three fields, because the two candidate
	// readings of the Windows stack disagree on more than a base address -
	// 0x32/0x33 with latch 0x01, or 0x64/0x67 (not adjacent) with latch 0x10.
	// Both are reachable from here without recompiling, which is the whole
	// point: the answer gets decided by a demodulator in front of a real
	// transmitter, not by more disassembly.
	char high[8], low[8], latch[8];
	snprintf(high, sizeof(high), "%02x", usb->FrequencyRegister());
	snprintf(low, sizeof(low), "%02x", usb->FrequencyRegisterLow());
	snprintf(latch, sizeof(latch), "%02x", usb->LatchValue());

	BWindow* panel = new BWindow(
		BRect(0, 0, 320, 190), "設定", B_MODAL_WINDOW,
		B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS);
	panel->CenterIn(Frame());

	BRadioButton* presetDefault = new BRadioButton("preset-default",
		"既定  32 / 33  ラッチ 01", NULL);
	BRadioButton* presetAlternate = new BRadioButton("preset-alt",
		"代替  64 / 67  ラッチ 10", NULL);
	BRadioButton* presetCustom = new BRadioButton("preset-custom",
		"手動指定 (16進):", NULL);

	BTextControl* field = new BTextControl("freqreg", "上位:", high, NULL);
	BTextControl* lowField = new BTextControl("freqreglow", "下位:", low, NULL);
	BTextControl* latchField = new BTextControl("latch", "ラッチ 0x42:", latch,
		NULL);

	// Whichever of the three the tuner is currently on starts selected, so the
	// panel always opens showing the truth rather than a default.
	if (usb->FrequencyRegister() == 0x32 && usb->FrequencyRegisterLow() == 0x33
		&& usb->LatchValue() == 0x01) {
		presetDefault->SetValue(B_CONTROL_ON);
	} else if (usb->FrequencyRegister() == 0x64
		&& usb->FrequencyRegisterLow() == 0x67 && usb->LatchValue() == 0x10) {
		presetAlternate->SetValue(B_CONTROL_ON);
	} else {
		presetCustom->SetValue(B_CONTROL_ON);
	}

	BStringView* hint = new BStringView("hint",
		"どれが正しいかは電波の前でしか決まりません - ファイルメニューの"
		"候補スイープが選択チャンネルで総当たりします。");

	BButton* ok = new BButton("ok", "OK", new BMessage(kMsgApplySettings));
	BButton* cancel = new BButton("cancel", "キャンセル",
		new BMessage(B_QUIT_REQUESTED));

	BLayoutBuilder::Group<>(panel, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_WINDOW_INSETS)
		.Add(presetDefault)
		.Add(presetAlternate)
		.Add(presetCustom)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.Add(field)
			.Add(lowField)
			.Add(latchField)
		.End()
		.Add(hint)
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
	lowField->SetTarget(this);
	latchField->SetTarget(this);

	// Remember the panel and controls so ApplySettings can read and close them.
	fSettingsPanel = panel;
	fSettingsField = field;
	fSettingsLowField = lowField;
	fSettingsLatchField = latchField;
	fPresetDefault = presetDefault;
	fPresetAlternate = presetAlternate;
	cancel->SetTarget(panel);

	panel->Show();
	field->MakeFocus(true);
}


uint8
MainWindow::HexByte(BTextControl* field, uint8 fallback)
{
	const char* text = field->Text();
	if (text == NULL || text[0] == '\0')
		return fallback;
	char* end = NULL;
	long value = strtol(text, &end, 16);
	if (end == text || value < 0 || value > 0xFF)
		return fallback;
	return (uint8)value;
}


void
MainWindow::ApplySettings(BMessage* message)
{
	(void)message;
	UsbTuner* usb = dynamic_cast<UsbTuner*>(fTuner);
	if (usb != NULL && fSettingsField != NULL && fSettingsLowField != NULL
		&& fSettingsLatchField != NULL) {
		// The two presets are the two readings of the vendor code; the manual
		// fields are for anything a sweep turns up that neither predicted. An
		// empty or unparseable field keeps what the tuner already has, so a
		// half-filled panel cannot silently zero a register.
		uint8 high, low, latch;
		if (fPresetDefault != NULL
			&& fPresetDefault->Value() == B_CONTROL_ON) {
			high = 0x32; low = 0x33; latch = 0x01;
		} else if (fPresetAlternate != NULL
			&& fPresetAlternate->Value() == B_CONTROL_ON) {
			high = 0x64; low = 0x67; latch = 0x10;
		} else {
			high = HexByte(fSettingsField, usb->FrequencyRegister());
			low = HexByte(fSettingsLowField, usb->FrequencyRegisterLow());
			latch = HexByte(fSettingsLatchField, usb->LatchValue());
		}

		usb->SetFrequencyRegisters(high, low);
		usb->SetLatchValue(latch);

		char status[128];
		snprintf(status, sizeof(status),
			"周波数レジスタ 0x%02x/0x%02x, ラッチ 0x%02x - 再スキャンしてください",
			high, low, latch);
		SetStatusText(status);
	}
	if (fSettingsPanel != NULL) {
		fSettingsPanel->PostMessage(B_QUIT_REQUESTED);
		fSettingsPanel = NULL;
		fSettingsField = NULL;
		fSettingsLowField = NULL;
		fSettingsLatchField = NULL;
		fPresetDefault = NULL;
		fPresetAlternate = NULL;
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

		case kMsgSweep:
			StartSweep();
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

		case kMsgSweepHit:
		{
			int32 index = 0, count = 0, high = 0, low = 0, latch = 0;
			bool signal = false, sync = false;
			int64 bytes = 0;
			message->FindInt32("index", &index);
			message->FindInt32("count", &count);
			message->FindInt32("high", &high);
			message->FindInt32("low", &low);
			message->FindInt32("latch", &latch);
			message->FindBool("signal", &signal);
			message->FindBool("sync", &sync);
			message->FindInt64("bytes", &bytes);

			BString log;
			log.SetToFormat("候補 %" B_PRId32 "/%" B_PRId32
				"  0x%02" B_PRIx32 "/0x%02" B_PRIx32 " ラッチ0x%02" B_PRIx32
				": ", index + 1, count, high, low, latch);
			if (bytes <= 0)
				log << "データなし";
			else if (!sync)
				log << bytes << " バイト, TS同期なし";
			else
				log << bytes << " バイト, TS同期 - 受信";
			SetStatusText(std::string(log.String()));
			break;
		}

		case kMsgSweepDone:
		{
			int32 hit = -1;
			message->FindInt32("hit", &hit);
			FinishScanUi();

			UsbTuner* usb = dynamic_cast<UsbTuner*>(fTuner);
			if (fQuitPending) {
				PostMessage(B_QUIT_REQUESTED);
				break;
			}
			if (hit >= 0 && usb != NULL) {
				// The sweep thread left the tuner on the layout that worked.
				BString log;
				log.SetToFormat("0x%02x/0x%02x ラッチ0x%02x で受信 - "
					"設定に適用しました", usb->FrequencyRegister(),
					usb->FrequencyRegisterLow(), usb->LatchValue());
				SetStatusText(std::string(log.String()));
				PostMessage(kMsgTune);
			} else {
				// Nothing received: put back what was in use, so a failed
				// sweep leaves no trace on the tuner.
				if (usb != NULL) {
					usb->SetFrequencyRegisters(fSweepSavedHigh, fSweepSavedLow);
					usb->SetLatchValue(fSweepSavedLatch);
				}
				SetStatusText(fScanCancel
					? "スイープ中止 - 設定は元のままです"
					: "どの候補でも受信できませんでした - アンテナと"
						"チャンネル選択を確認してください");
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
