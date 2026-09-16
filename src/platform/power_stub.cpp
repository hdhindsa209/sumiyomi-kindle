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

PowerEvents::~PowerEvents() { stop(); }

bool PowerEvents::start(std::string& err)
{
    // Nothing on the host sleeps the machine out from under us, so there is nothing to listen to.
    err = "no powerd on the host build";
    return false;
}

void PowerEvents::stop() noexcept
{
    fd_  = -1;
    pid_ = -1;
}
void PowerEvents::drain(const std::function<void(Event)>&) {}

} // namespace sumi
