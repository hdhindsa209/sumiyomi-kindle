#pragma once
#include <string>

namespace sumi {

// Removes Sumiyomi from the device, from inside Sumiyomi (More -> Uninstall).
//
// It can't delete itself while it is running — the binary is mapped, the data directory is open —
// so this hands the job to a small detached shell script that waits for this process to exit and
// then deletes what it was told to. The caller quits immediately afterwards.
//
// `keep_data` keeps /mnt/us/sumiyomi (library, read progress, downloads, installed sources) and
// removes only the app, so reinstalling carries on where the user left off.
//
// Returns false with `err` if the helper couldn't be started; nothing is deleted in that case.
bool schedule_uninstall(bool keep_data, std::string& err);

} // namespace sumi
