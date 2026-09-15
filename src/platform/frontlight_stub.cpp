// Host / simulator: no hardware light.
#include "platform/frontlight.h"

namespace sumi {

std::unique_ptr<Frontlight> make_frontlight() { return std::make_unique<FakeFrontlight>(); }

} // namespace sumi
