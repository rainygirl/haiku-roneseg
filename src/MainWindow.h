#ifndef RONESEG_MAIN_WINDOW_H
#define RONESEG_MAIN_WINDOW_H

#include <Messenger.h>
#include <Window.h>

#include <string>
#include <vector>

#include "ChannelTable.h"

class BButton;
class BListView;
class BMenuBar;
class BStringView;
class BSlider;
class BView;
class BWindow;
class Player;
class Tuner;
class VideoView;

class UsbTuner;

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
	void StartTuning(int32 selected);
	void ShowUsbReport();
	void StartScan();
	void ToggleMeter();
	void ShowDiagnostic();
	void CloseDiagnostic();
	// True (and says so) while the diagnostic window holds the tuner.
	bool DiagnosticBusy();
	void FinishScanUi();
	void SetStatusText(const std::string& text);
	void ToggleFullscreen();

	static status_t ScanEntry(void* self);
	static status_t TuneEntry(void* self);
	static status_t MeterEntry(void* self);

	std::vector<ChannelTable::Channel>	fChannels;
	std::string							fCapturePath;

	BMenuBar*		fMenuBar;
	BListView*		fChannelList;
	BButton*		fScanButton;
	VideoView*		fVideoView;
	BStringView*	fStatusView;
	BSlider*		fVolumeSlider;
	BView*			fSidebar;
	BView*			fControls;
	bool			fFullscreen;
	bool			fWindowedScaled;
	BRect			fWindowedFrame;
	Player*			fPlayer;
	Tuner*			fTuner;

	// The scan and the signal meter are the same kind of job - one thread
	// holding the tuner, cancellable, quit deferred until it stops - so they
	// share this state and only one can run at a time.
	thread_id		fScanThread;
	thread_id		fTuneThread;
	int32			fTuneCancel;
	int32			fTuneIndex;
	int32			fPendingTune;
	volatile bool	fScanCancel;
	bool			fQuitPending;
	int32			fScanFirstHit;
	int32			fScanStartIndex;

	bool			fMeterRunning;			// the worker is the signal meter
	int32			fMeterChannel;			// list index the meter tunes

	// The tuner diagnostic borrows the USB tuner; in capture mode it gets
	// one of its own. Nothing else may touch the tuner while it is open.
	BMessenger		fDiagnostic;
	UsbTuner*		fDiagnosticTuner;
};

#endif
