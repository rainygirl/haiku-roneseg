#ifndef RONESEG_DIAGNOSTIC_WINDOW_H
#define RONESEG_DIAGNOSTIC_WINDOW_H

#include <Window.h>

#include <string>

class BButton;
class BTextView;
class UsbTuner;

// Runs TunerDiagnostic on its own thread and shows the report as it arrives.
// The report is also written to ~/config/settings/roneseg/diagnosis.txt when
// the run finishes, so it can be sent to someone without retyping it.
class DiagnosticWindow : public BWindow {
public:
	// The tuner is borrowed; the caller must not use it until this window is
	// gone (MainWindow checks IsRunning() through its messenger).
	DiagnosticWindow(BRect frame, UsbTuner* tuner);
	virtual ~DiagnosticWindow();

	virtual void MessageReceived(BMessage* message);
	virtual bool QuitRequested();

private:
	static status_t	RunEntry(void* cookie);
	static void		Line(const std::string& line, void* cookie);
	void			SaveReport();

	UsbTuner*		fTuner;
	BTextView*		fText;
	BButton*		fCloseButton;
	thread_id		fThread;
	volatile bool	fCancel;
	std::string		fReport;
};

#endif
