#include "VideoView.h"

#include <Window.h>

VideoView::VideoView()
	:
	// B_FULL_UPDATE_ON_RESIZE: the fitted rectangle moves and grows with the
	// view, so a resize must repaint all of it, not just the newly exposed
	// strip.
	BView("video", B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE),
	fLock("video-frame"),
	fFrame(NULL),
	fPlaceholder("no signal"),
	fScaled(true),
	fBarCount(0),
	fBarPos(0),
	fCropLeft(0),
	fCropRight(0)
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
VideoView::FrameResized(float width, float height)
{
	BView::FrameResized(width, height);
	Invalidate();
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
		fBarCount = 0;
		fBarPos = 0;
		fCropLeft = 0;
		fCropRight = 0;
	}
	if (fFrame != NULL && fFrame->InitCheck() == B_OK) {
		size_t length = source->BitsLength();
		if (length > (size_t)fFrame->BitsLength())
			length = fFrame->BitsLength();
		memcpy(fFrame->Bits(), source->Bits(), length);
		MeasureSideBars();
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
	fBarCount = 0;
	fBarPos = 0;
	fCropLeft = 0;
	fCropRight = 0;
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


// A pillarbox column is dark on average, allowing a few bright samples:
// compression ringing and the odd edge line otherwise stopped the scan one
// column in. Haiku's YCbCr->RGB conversion left TNU's right-hand bar at
// about 28-40, so a strict per-pixel threshold missed the whole bar.
static bool
IsBarColumn(const uint8* bits, int32 bpr, int height, int x)
{
	int samples = 0;
	int bright = 0;
	int sum = 0;
	for (int y = 0; y < height; y += 4) {
		const uint8* p = bits + y * bpr + x * 4;
		int v = p[0] > p[1] ? p[0] : p[1];
		if (p[2] > v)
			v = p[2];
		sum += v;
		if (v >= 64)
			bright++;
		samples++;
	}
	return samples > 0 && sum < 28 * samples && bright * 20 <= samples;
}


// Width of the bar at one edge. The outermost column or two can be an
// encoder edge artefact: TNU's column 319 is a flat grey around 29 while
// 318 inwards is true black, so a scan that must start at the very edge
// found no right-hand bar in half the frames.
static int
BarWidth(const uint8* bits, int32 bpr, int width, int height, int limit,
	bool fromRight)
{
	for (int skip = 0; skip <= 2; skip++) {
		int run = 0;
		while (skip + run < limit) {
			int x = skip + run;
			if (fromRight)
				x = width - 1 - x;
			if (!IsBarColumn(bits, bpr, height, x))
				break;
			run++;
		}
		if (run >= 4)
			return skip + run;
	}
	return 0;
}


// Called with fLock held, on the frame just copied in.
void
VideoView::MeasureSideBars()
{
	const int width = (int)fFrame->Bounds().Width() + 1;
	const int height = (int)fFrame->Bounds().Height() + 1;
	const int32 bpr = fFrame->BytesPerRow();
	const uint8* bits = (const uint8*)fFrame->Bits();
	const int limit = width / 4;

	int left = BarWidth(bits, bpr, width, height, limit, false);
	int right = BarWidth(bits, bpr, width, height, limit, true);

	fBarLeft[fBarPos] = left;
	fBarRight[fBarPos] = right;
	fBarPos = (fBarPos + 1) % kBarHistory;
	if (fBarCount < kBarHistory)
		fBarCount++;
	// Wait for a second of history before cropping at all.
	if (fBarCount < 15)
		return;

	int cropLeft = limit;
	int cropRight = limit;
	for (int i = 0; i < fBarCount; i++) {
		if (fBarLeft[i] < cropLeft)
			cropLeft = fBarLeft[i];
		if (fBarRight[i] < cropRight)
			cropRight = fBarRight[i];
	}
	// A pixel or two of edge darkness is not a bar.
	fCropLeft = cropLeft >= 4 ? cropLeft : 0;
	fCropRight = cropRight >= 4 ? cropRight : 0;
}


// Caller holds fLock and has checked fFrame.
BRect
VideoView::SourceRect() const
{
	BRect source = fFrame->Bounds();
	if (fScaled) {
		source.left += fCropLeft;
		source.right -= fCropRight;
	}
	return source;
}


BRect
VideoView::FrameRect() const
{
	// Caller holds fLock and has checked fFrame.
	BRect source = SourceRect();
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

	DrawBitmap(fFrame, SourceRect(), destination);

	fLock.Unlock();
	(void)updateRect;
}
