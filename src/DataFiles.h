#ifndef RONESEG_DATA_FILES_H
#define RONESEG_DATA_FILES_H

#include <Application.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Roster.h>
#include <string>
#include <unistd.h>

// Prefer the data installed beside this executable, so package upgrades and
// source installations do not accidentally use an older settings copy.
inline std::string
OneSegDataFile(const char* name)
{
	BPath path;
	app_info info;
	if (be_app != NULL && be_app->GetAppInfo(&info) == B_OK
		&& path.SetTo(&info.ref) == B_OK
		&& path.GetParent(&path) == B_OK
		&& path.GetParent(&path) == B_OK
		&& path.Append("data/roneseg") == B_OK
		&& path.Append(name) == B_OK && access(path.Path(), R_OK) == 0)
		return path.Path();
	const directory_which directories[] = {
		B_USER_NONPACKAGED_DATA_DIRECTORY, B_USER_DATA_DIRECTORY,
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY, B_SYSTEM_DATA_DIRECTORY,
		B_USER_SETTINGS_DIRECTORY
	};
	for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
		if (find_directory(directories[i], &path) == B_OK
			&& path.Append("roneseg") == B_OK && path.Append(name) == B_OK
			&& access(path.Path(), R_OK) == 0)
			return path.Path();
	}
	return std::string();
}

#endif
