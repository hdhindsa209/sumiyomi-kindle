// Host build: no framework, no wakelock.
#include "platform/power.h"

#include "core/log.h"

namespace sumi {

bool PowerGuard::acquire(std::string& /*err*/)
{
    SUMI_LOGI("power", "stub: nothing to acquire on host");
    return true;
}

void PowerGuard::restore() noexcept {}

bool PowerGuard::sleep(const std::vector<int>& /*wake_fds*/, std::string& err)
{
    // The simulator has no device to suspend. main() draws the sleep screen before calling this and
    // repaints when it returns, so the sleep screen still appears here for a moment — enough to see
    // what it looks like, but the real behaviour only exists on the Kindle.
    err = "no device to suspend on the host build";
    return false;
}

} // namespace sumi
