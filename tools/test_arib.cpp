// Unit tests for the ARIB STD-B24 text decoder.
//
// Service names are the one place this app shows broadcast-authored text, and
// getting the encoding wrong does not fail loudly - it produces a plausible
// wrong string. So the cases that a synthetic test stream does *not* exercise
// are exactly the ones worth testing here: make_test_ts.py writes everything
// in the two-byte kanji plane, which is the decoder's default state, so a
// hardware test that passes proves only that the default path works.
//
// The escape sequences below are what real broadcasters use for names with
// single-byte kana or latin letters, and one of them ("ESC ( 1" for katakana)
// was decoded as alphanumeric until this test was written.
//
// Runs on Haiku:
//   setarch x86 g++ -o test_arib tools/test_arib.cpp src/AribText.cpp \
//       -liconv -lstdc++ -lsupc++
//   ./test_arib
//
// Also runs anywhere else with an iconv, which is the point - it needs no
// hardware and no Media Kit, so it can be run while the machine is off. Off
// Haiku, supply a SupportDefs.h that typedefs uint8/uint16/uint32/int32 and
// build with -I pointing at it.

#include "../src/AribText.h"

#include <iconv.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

static int sFailures = 0;


// UTF-8 to the bytes a broadcaster would write for the two-byte kanji plane:
// EUC-JP with bit 7 cleared on every byte.
static std::vector<uint8>
Jis(const char* utf8)
{
	std::vector<uint8> out;
	iconv_t converter = iconv_open("EUC-JP", "UTF-8");
	if (converter == (iconv_t)-1)
		return out;

	std::string in(utf8);
	char* input = &in[0];
	size_t inputLeft = in.size();
	char buffer[256];
	char* output = buffer;
	size_t outputLeft = sizeof(buffer);

	iconv(converter, &input, &inputLeft, &output, &outputLeft);
	iconv_close(converter);

	for (char* p = buffer; p < output; p++)
		out.push_back((uint8)*p & 0x7F);
	return out;
}


static void
Append(std::vector<uint8>& v, const std::vector<uint8>& more)
{
	v.insert(v.end(), more.begin(), more.end());
}


static void
Check(const char* what, const std::vector<uint8>& in, const char* expect)
{
	std::string got = AribText::ToUtf8(in.empty() ? NULL : &in[0], in.size());
	bool ok = got == expect;
	if (!ok)
		sFailures++;
	printf("%-40s %s\n", what, ok ? "pass" : "FAIL");
	if (!ok)
		printf("    got \"%s\", expected \"%s\"\n", got.c_str(), expect);
}


int
main()
{
	// The default state: two-byte kanji plane in GL, no escapes at all. Kana
	// live in that plane too (rows 4 and 5), which is why a name like this
	// needs no escape and why the hardware test passed without exercising
	// any of the cases below.
	Check("kanji and katakana, default G0", Jis("テスト放送"), "テスト放送");

	// ESC ( J - alphanumeric. Names like "NHK G" arrive this way.
	{
		std::vector<uint8> v;
		v.push_back(0x1B); v.push_back(0x28); v.push_back(0x4A);
		for (const char* p = "NHK G"; *p != '\0'; p++)
			v.push_back((uint8)*p);
		Check("ESC ( J, alphanumeric", v, "NHK G");
	}

	// ESC ( 0 - the single-byte hiragana set. One byte per character,
	// indexing row 4 of JIS X 0208.
	{
		std::vector<uint8> v;
		v.push_back(0x1B); v.push_back(0x28); v.push_back(0x30);
		v.push_back(0x22); v.push_back(0x24); v.push_back(0x26);
		Check("ESC ( 0, single-byte hiragana", v, "あいう");
	}

	// ESC ( 1 - the single-byte katakana set, row 5. This is the case the
	// decoder got wrong: the final byte fell through to alphanumeric and the
	// name came out as punctuation.
	{
		std::vector<uint8> v;
		v.push_back(0x1B); v.push_back(0x28); v.push_back(0x31);
		v.push_back(0x22); v.push_back(0x24); v.push_back(0x26);
		Check("ESC ( 1, single-byte katakana", v, "アイウ");
	}

	// ESC $ B - back to the two-byte plane, three-byte form.
	{
		std::vector<uint8> v;
		v.push_back(0x1B); v.push_back(0x28); v.push_back(0x4A);
		v.push_back('A');
		v.push_back(0x1B); v.push_back(0x24); v.push_back(0x42);
		Append(v, Jis("放送"));
		Check("ESC ( J then ESC $ B", v, "A放送");
	}

	// ESC $ ( B - the same designation in its four-byte form. Measuring this
	// as three bytes leaves the parser on the final byte and emits a stray
	// character before the real text.
	{
		std::vector<uint8> v;
		v.push_back(0x1B); v.push_back(0x24); v.push_back(0x28);
		v.push_back(0x42);
		Append(v, Jis("放送"));
		Check("ESC $ ( B, four-byte form", v, "放送");
	}

	// SS2 invokes G2 - hiragana in ARIB's default setup - for exactly one
	// character, after which GL reverts on its own.
	{
		std::vector<uint8> v;
		Append(v, Jis("放"));
		v.push_back(0x19); v.push_back(0x22);
		Append(v, Jis("送"));
		Check("SS2, one hiragana then back", v, "放あ送");
	}

	// Truncation. A weak signal delivers half a descriptor as readily as a
	// whole one, and none of these may read past the end or spin.
	{
		std::vector<uint8> v;
		v.push_back(0x1B);
		Check("lone ESC at end of input", v, "");
	}
	{
		std::vector<uint8> v;
		Append(v, Jis("放"));
		v.push_back(0x30);
		Check("odd trailing byte, half a pair", v, "放");
	}
	Check("empty input", std::vector<uint8>(), "");

	printf("\n%s\n", sFailures == 0 ? "all passed" : "FAILURES ABOVE");
	return sFailures == 0 ? 0 : 1;
}
