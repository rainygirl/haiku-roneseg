#ifndef RONESEG_ARIB_TEXT_H
#define RONESEG_ARIB_TEXT_H

#include <SupportDefs.h>

#include <string>

// Decodes ARIB STD-B24 text - the encoding ISDB-T uses for every human-
// readable string in its service information - into UTF-8.
//
// The full standard is large (DRCS, mosaic character sets, colour and
// positioning control codes for subtitles). This handles what service and
// event names actually use: the two-byte kanji plane, hiragana, katakana,
// and the single-byte alphanumeric set, plus the escape sequences that
// switch between them.
//
// The kanji plane is JIS X 0208, which is the same plane EUC-JP carries with
// bit 7 set on both bytes. So rather than shipping a 7000-entry conversion
// table, this sets that bit back and hands the result to iconv, which Haiku
// already has an EUC-JP converter for. Hiragana and katakana are rows 0x24
// and 0x25 of the same plane, so they go the same way.
namespace AribText {

// Never fails: anything undecodable is replaced rather than dropped, because
// a channel with a mangled name is still selectable and a channel with no
// name is not.
std::string ToUtf8(const uint8* data, size_t size);

} // namespace AribText

#endif
