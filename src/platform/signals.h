#pragma once
#include <string>

namespace sumi {

class Display;
class PowerGuard;

// Installs handlers for SIGTERM, SIGINT, SIGHUP, SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE (§8).
// Each handler: raw log line, display->clear_screen() (if non-null), guard->restore(),
// _exit(128 + sig). Both pointers must outlive the process's use of signals.
bool install_signal_handlers(PowerGuard* guard, Display* display, std::string& err);

} // namespace sumi
