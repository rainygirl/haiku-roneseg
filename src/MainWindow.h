#ifndef RONESEG_MAIN_WINDOW_H
#define RONESEG_MAIN_WINDOW_H

#include <Window.h>

#include <string>
#include <vector>

#include "ChannelTable.h"

class BButton;
class BListView;
class BMenuBar;
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
	void StartScan();
	void SetStatusText(const std::string& text);

	static status_t ScanEntry(void* self);

	std::vector<ChannelTable::Channel>	fChannels;
	std::string							fCapturePath;

	BMenuBar*		fMenuBar;
	BListView*		fChannelList;
	BButton*		fScanButton;
	VideoView*		fVideoView;
	BStringView*	fStatusView;
	BWindow*		fSettingsPanel;
	BTextControl*	fSettingsField;
	Player*			fPlayer;
	Tuner*			fTuner;

	thread_id		fScanThread;
	volatile bool	fScanCancel;
	bool			fQuitPending;
	int32			fScanFirstHit;
};

#endif
