#pragma once
#include <string>

namespace sumi {

class PowerGuard;

// Installs handlers for SIGTERM, SIGINT, SIGHUP, SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE (§8).
// Each handler: raw log line, guard->restore(), _exit(128 + sig).
// `guard` must outlive the process's use of signals.
bool install_signal_handlers(PowerGuard* guard, std::string& err);

} // namespace sumi
