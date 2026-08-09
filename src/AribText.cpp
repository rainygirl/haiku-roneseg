#include "AribText.h"

#include <SupportDefs.h>

#include <errno.h>
#include <iconv.h>
#include <string.h>

namespace AribText {

namespace {

// The graphic sets this decoder knows. ARIB designates a set into one of
// four registers (G0..G3) and then maps a register into GL or GR; in
// practice service names stay in GL and only change which set is there.
enum GraphicSet {
	kKanji,			// two bytes, JIS X 0208
	kAlphanumeric,	// one byte, ASCII-positioned
	kHiragana,		// one byte, JIS X 0208 row 0x24
	kKatakana		// one byte, JIS X 0208 row 0x25
};


// The final byte of a designation escape names the graphic set. These are
// the ones that appear in service and event names; anything else falls back
// to the caller's default rather than being guessed at.
GraphicSet
SetForFinalByte(uint8 final, GraphicSet fallback)
{
	switch (final) {
		case 0x4A:	// alphanumeric
			return kAlphanumeric;
		case 0x30:	// hiragana
			return kHiragana;
		case 0x31:	// katakana
			return kKatakana;
		case 0x42:	// kanji, two-byte
		case 0x39:	// JIS compatible kanji plane 1
		case 0x3A:	// plane 2
		case 0x3B:	// additional symbols
			return kKanji;
		default:
			return fallback;
	}
}


// Convert a run of JIS X 0208 code points (as 7-bit byte pairs) to UTF-8 by
// promoting them to EUC-JP and letting iconv do the table lookup.
void
AppendJis(std::string& out, const std::string& jisPairs)
{
	if (jisPairs.empty())
		return;

	std::string euc(jisPairs);
	for (size_t i = 0; i < euc.size(); i++)
		euc[i] = (char)((uint8)euc[i] | 0x80);

	iconv_t converter = iconv_open("UTF-8", "EUC-JP");
	if (converter == (iconv_t)-1) {
		out.append(jisPairs.size() / 2, '?');
		return;
	}

	char* input = &euc[0];
	size_t inputLeft = euc.size();
	char buffer[512];

	while (inputLeft > 0) {
		char* output = buffer;
		size_t outputLeft = sizeof(buffer);
		size_t result = iconv(converter, &input, &inputLeft, &output,
			&outputLeft);
		out.append(buffer, sizeof(buffer) - outputLeft);

		if (result == (size_t)-1) {
			if (errno == E2BIG)
				continue;	// buffer full, drain and carry on
			// An unconvertible pair: skip it rather than abandoning the
			// whole name, and keep the position so the rest still lines up.
			if (inputLeft >= 2) {
				input += 2;
				inputLeft -= 2;
				out += '?';
				continue;
			}
			break;
		}
	}

	iconv_close(converter);
}

} // namespace


std::string
ToUtf8(const uint8* data, size_t size)
{
	std::string out;
	// Pending kanji-plane bytes, converted in one iconv run per contiguous
	// run rather than per character.
	std::string jis;

	// ARIB's initial designation for the 8-bit form: kanji in G0 and mapped
	// into GL, which is why a name made only of kanji and kana carries no
	// escape sequences at all.
	GraphicSet current = kKanji;

	size_t i = 0;
	while (i < size) {
		uint8 byte = data[i];

		// ESC: a designation change. Only the single-byte final designators
		// that matter for names are recognised; anything else is skipped
		// along with its parameters so the rest of the string survives.
		if (byte == 0x1B) {
			if (i + 1 >= size)
				break;
			uint8 next = data[i + 1];
			if (next == 0x24) {
				// ESC 02/04 ... - a two-byte set.
				//
				// Two shapes: "ESC $ F" designates directly into G0, while
				// "ESC $ ( F" and friends name a register first. Only the G0
				// form changes what GL decodes, but both have to be measured
				// correctly or the parser resumes mid-escape and turns the
				// rest of the name into garbage.
				bool named = i + 2 < size && data[i + 2] >= 0x28
					&& data[i + 2] <= 0x2B;
				if (!named) {
					AppendJis(out, jis);
					jis.clear();
					current = kKanji;	// every two-byte set here is JIS X 0208
				}
				i += named ? 4 : 3;
				continue;
			}
			if (next == 0x28 || next == 0x29 || next == 0x2A || next == 0x2B) {
				// ESC 02/08..02/11 xx - designate a one-byte set. Only the
				// designation into G0 (02/08) changes what GL decodes,
				// because nothing here ever maps another register into GL;
				// the others are tracked only so their bytes are skipped.
				uint8 final = (i + 2 < size) ? data[i + 2] : 0;
				if (next == 0x28) {
					AppendJis(out, jis);
					jis.clear();
					current = SetForFinalByte(final, kAlphanumeric);
				}
				i += 3;
				continue;
			}
			i += 2;
			continue;
		}

		// Shift-out / shift-in and the single shifts select between the
		// registers. Names use these to drop a katakana or hiragana run into
		// an otherwise-kanji string.
		if (byte == 0x0F) {	// LS0
			AppendJis(out, jis);
			jis.clear();
			current = kKanji;
			i++;
			continue;
		}
		if (byte == 0x0E) {	// LS1
			AppendJis(out, jis);
			jis.clear();
			current = kAlphanumeric;
			i++;
			continue;
		}
		if (byte == 0x19 || byte == 0x1D) {	// SS2 / SS3: one character only
			if (i + 1 < size) {
				uint8 single = data[i + 1] & 0x7F;
				// SS2 is hiragana, SS3 katakana, in ARIB's default setup.
				uint8 row = (byte == 0x19) ? 0x24 : 0x25;
				jis += (char)row;
				jis += (char)single;
				i += 2;
			} else {
				i++;
			}
			continue;
		}

		// Other C0 control codes have no place in a name.
		if (byte < 0x20) {
			i++;
			continue;
		}

		switch (current) {
			case kKanji:
				if (i + 1 < size) {
					jis += (char)(data[i] & 0x7F);
					jis += (char)(data[i + 1] & 0x7F);
					i += 2;
				} else {
					i++;
				}
				break;

			case kHiragana:
			case kKatakana:
				jis += (char)(current == kHiragana ? 0x24 : 0x25);
				jis += (char)(byte & 0x7F);
				i++;
				break;

			case kAlphanumeric:
			default:
				AppendJis(out, jis);
				jis.clear();
				if (byte >= 0x20 && byte < 0x7F)
					out += (char)byte;
				i++;
				break;
		}
	}

	AppendJis(out, jis);
	return out;
}

} // namespace AribText
