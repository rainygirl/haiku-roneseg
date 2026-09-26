#ifndef RONESEG_VIDEO_VIEW_H
#define RONESEG_VIDEO_VIEW_H

#include <Bitmap.h>
#include <Locker.h>
#include <View.h>

#include <string>

// Draws whatever frame the decoder most recently produced, letterboxed.
//
// One-Seg is 320x240 at 15fps. On this machine there is no graphics
// acceleration at all - the GMA500 has no Haiku driver, so app_server is
// software-compositing into a VESA framebuffer - which makes the scale
// factor the single most expensive thing on screen. Drawing 320x240 into a
// 1600x768 window means the CPU touches 25 times as many pixels as the
// decoder produced.
//
// So: aspect is preserved (a stretched broadcast picture is wrong, and the
// VAIO P panel is far wider than 4:3, so stretching to fill would be very
// wrong). By default the picture is fitted to the view and refitted whenever
// the window is resized - 320x180 drawn 1:1 was a postage stamp on the
// 1600x768 panel. Alt-F switches to 1:1, which turns scaling off entirely
// for when the Atom cannot keep up.
class VideoView : public BView {
public:
	VideoView();
	virtual ~VideoView();

	virtual void Draw(BRect updateRect);
	virtual void AttachedToWindow();
	virtual void FrameResized(float width, float height);

	// Copies the frame in. Called from the decode thread.
	//
	// Copying rather than taking ownership so the decoder does not have to
	// allocate a bitmap per frame: at 320x240 in RGB32 that is 300 kB
	// allocated and freed fifteen times a second, which on a CPU with one
	// thread and no video acceleration is time the decoder cannot spare.
	// The copy happens under the same lock Draw() holds, which is also what
	// stops app_server reading a bitmap while it is being overwritten.
	void SetFrame(const BBitmap* source);
	void Clear();

	// Shown centred when there is no picture: "no signal", an error, etc.
	void SetPlaceholder(const std::string& text);

	void SetScaled(bool scaled);
	bool IsScaled() const { return fScaled; }

private:
	BRect FrameRect() const;
	// Source rectangle without the broadcaster's side bars.
	BRect SourceRect() const;
	void MeasureSideBars();

	mutable BLocker	fLock;
	BBitmap*		fFrame;
	std::string		fPlaceholder;
	bool			fScaled;

	// Black side bars inside the picture, e.g. TNU sends 4:3 pillarboxed in
	// a 320x180 frame. A column only counts as a bar if it was black in every
	// one of the recent frames, so a dark scene is never mistaken for one.
	static const int kBarHistory = 45;		// ~3 s at 15 fps
	int				fBarLeft[kBarHistory];
	int				fBarRight[kBarHistory];
	int				fBarCount;
	int				fBarPos;
	int				fCropLeft;
	int				fCropRight;
};

#endif
