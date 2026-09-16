#include "platform/uninstall.h"

#include "core/log.h"

namespace sumi {

bool schedule_uninstall(bool keep_data, std::string& err)
{
    // Deliberately does nothing on the host: the "installation" here is the source tree, and a
    // simulator run must never be able to delete it. The UI path is still exercised — the app quits,
    // it just comes back next time.
    SUMI_LOGI("uninstall", "stub: would remove the app%s", keep_data ? " (keeping the library)" : " and its data");
    err.clear();
    return true;
}

} // namespace sumi
