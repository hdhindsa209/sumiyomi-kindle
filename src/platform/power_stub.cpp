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

} // namespace sumi
