#ifndef RONESEG_MAIN_WINDOW_H
#define RONESEG_MAIN_WINDOW_H

#include <Window.h>

#include <string>
#include <vector>

#include "ChannelTable.h"

class BButton;
class BListView;
class BMenuBar;
class BRadioButton;
class BStringView;
class BTextControl;
class BWindow;
class Player;
class Tuner;
class VideoView;

class MainWindow : public BWindow {
public:
	// capturePath: when non-empty, the file backend replays that .ts instead
	// of using the (unidentified) internal tuner. This is how the app is
	// usable at all today - see README.md.
	explicit MainWindow(const std::string& capturePath);
	virtual ~MainWindow();

	virtual void MessageReceived(BMessage* message);
	virtual bool QuitRequested();

private:
	void BuildLayout();
	void BuildMenu();
	void TuneToSelection();
	void ShowUsbReport();
	void ShowSettings();
	void ApplySettings(BMessage* message);
	static uint8 HexByte(BTextControl* field, uint8 fallback);
	void StartScan();
	void StartSweep();
	void FinishScanUi();
	void SetStatusText(const std::string& text);

	static status_t ScanEntry(void* self);
	static status_t SweepEntry(void* self);

	std::vector<ChannelTable::Channel>	fChannels;
	std::string							fCapturePath;

	BMenuBar*		fMenuBar;
	BListView*		fChannelList;
	BButton*		fScanButton;
	VideoView*		fVideoView;
	BStringView*	fStatusView;
	BWindow*		fSettingsPanel;
	BTextControl*	fSettingsField;			// frequency register, high byte
	BTextControl*	fSettingsLowField;		// frequency register, low byte
	BTextControl*	fSettingsLatchField;	// value pulsed into demod 0x42
	BRadioButton*	fPresetDefault;			// 0x32/0x33, latch 0x01
	BRadioButton*	fPresetAlternate;		// 0x64/0x67, latch 0x10
	Player*			fPlayer;
	Tuner*			fTuner;

	// The scan and the candidate sweep are the same kind of job - one thread
	// holding the tuner, cancellable, quit deferred until it stops - so they
	// share this state and only one can run at a time.
	thread_id		fScanThread;
	volatile bool	fScanCancel;
	bool			fQuitPending;
	int32			fScanFirstHit;

	int32			fSweepChannel;			// list index the sweep tunes
	uint8			fSweepSavedHigh;		// layout to restore if it fails
	uint8			fSweepSavedLow;
	uint8			fSweepSavedLatch;
};

#endif
