#ifndef RONESEG_LOCALIZE_H
#define RONESEG_LOCALIZE_H

// The UI is in Japanese when the system's preferred language is Japanese,
// and in English otherwise.
bool IsJapaneseLocale();

inline const char*
Tr(const char* japanese, const char* english)
{
	return IsJapaneseLocale() ? japanese : english;
}

#endif
