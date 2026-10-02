#include "Localize.h"

#include <LocaleRoster.h>
#include <Message.h>
#include <String.h>


bool
IsJapaneseLocale()
{
	static int sJapanese = -1;
	if (sJapanese < 0) {
		BMessage languages;
		const char* first = NULL;
		sJapanese = BLocaleRoster::Default()->GetPreferredLanguages(&languages)
				== B_OK
			&& languages.FindString("language", 0, &first) == B_OK
			&& BString(first).StartsWith("ja") ? 1 : 0;
	}
	return sJapanese == 1;
}
