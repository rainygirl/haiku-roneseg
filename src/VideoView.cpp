#include "VideoView.h"

#include <Window.h>

VideoView::VideoView()
	:
	BView("video", B_WILL_DRAW | B_FRAME_EVENTS),
	fLock("video-frame"),
	fFrame(NULL),
	fPlaceholder("no signal"),
	fScaled(false)
{
	SetViewColor(0, 0, 0);
	SetLowColor(0, 0, 0);
}


VideoView::~VideoView()
{
	delete fFrame;
}


void
VideoView::AttachedToWindow()
{
	SetViewColor(0, 0, 0);
}


void
VideoView::SetFrame(const BBitmap* source)
{
	if (source == NULL)
		return;

	fLock.Lock();
	if (fFrame == NULL || fFrame->Bounds() != source->Bounds()) {
		delete fFrame;
		fFrame = new(std::nothrow) BBitmap(source->Bounds(), B_RGB32);
	}
	if (fFrame != NULL && fFrame->InitCheck() == B_OK) {
		size_t length = source->BitsLength();
		if (length > (size_t)fFrame->BitsLength())
			length = fFrame->BitsLength();
		memcpy(fFrame->Bits(), source->Bits(), length);
	}
	fLock.Unlock();

	// Invalidate() is safe from another thread only with the window locked.
	// LockLooperWithTimeout rather than LockLooper: the decode thread must
	// not stall behind a busy window, and dropping the repaint for one frame
	// of a 15fps stream is invisible.
	if (LockLooperWithTimeout(10000) == B_OK) {
		Invalidate();
		UnlockLooper();
	}
}


void
VideoView::Clear()
{
	fLock.Lock();
	delete fFrame;
	fFrame = NULL;
	fLock.Unlock();

	if (LockLooperWithTimeout(10000) == B_OK) {
		Invalidate();
		UnlockLooper();
	}
}


void
VideoView::SetPlaceholder(const std::string& text)
{
	fLock.Lock();
	fPlaceholder = text;
	fLock.Unlock();

	if (LockLooperWithTimeout(10000) == B_OK) {
		Invalidate();
		UnlockLooper();
	}
}


void
VideoView::SetScaled(bool scaled)
{
	fScaled = scaled;
	Invalidate();
}


BRect
VideoView::FrameRect() const
{
	// Caller holds fLock and has checked fFrame.
	BRect source = fFrame->Bounds();
	BRect bounds = Bounds();

	float width = source.Width() + 1;
	float height = source.Height() + 1;

	if (fScaled) {
		float scale = bounds.Width() / width;
		float verticalScale = bounds.Height() / height;
		if (verticalScale < scale)
			scale = verticalScale;
		width *= scale;
		height *= scale;
	}

	float x = (bounds.Width() - width) / 2;
	float y = (bounds.Height() - height) / 2;
	return BRect(x, y, x + width - 1, y + height - 1);
}


void
VideoView::Draw(BRect updateRect)
{
	fLock.Lock();

	if (fFrame == NULL) {
		std::string text = fPlaceholder;
		fLock.Unlock();

		SetHighColor(140, 140, 140);
		font_height height;
		GetFontHeight(&height);
		float width = StringWidth(text.c_str());
		BRect bounds = Bounds();
		DrawString(text.c_str(),
			BPoint((bounds.Width() - width) / 2,
				(bounds.Height() + height.ascent) / 2));
		return;
	}

	BRect destination = FrameRect();

	// Only paint the letterbox bars that the picture does not cover, rather
	// than filling the whole view first and drawing over it. At 15fps on a
	// software framebuffer, painting 1600x768 of black twice per frame is
	// real time that the decoder needs.
	BRect bounds = Bounds();
	SetHighColor(0, 0, 0);
	if (destination.top > bounds.top)
		FillRect(BRect(bounds.left, bounds.top, bounds.right, destination.top - 1));
	if (destination.bottom < bounds.bottom)
		FillRect(BRect(bounds.left, destination.bottom + 1, bounds.right, bounds.bottom));
	if (destination.left > bounds.left)
		FillRect(BRect(bounds.left, destination.top, destination.left - 1, destination.bottom));
	if (destination.right < bounds.right)
		FillRect(BRect(destination.right + 1, destination.top, bounds.right, destination.bottom));

	DrawBitmap(fFrame, fFrame->Bounds(), destination);

	fLock.Unlock();
	(void)updateRect;
}
