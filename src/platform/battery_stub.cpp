#include "platform/battery.h"

namespace sumi {

std::unique_ptr<Battery> make_battery() { return std::make_unique<FakeBattery>(); }

} // namespace sumi
