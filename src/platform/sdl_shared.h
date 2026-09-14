#pragma once
#include <functional>

namespace sumi {

// Set by the SDL display while open; called by the SDL input backend when the window
// needs repainting (expose/resize), since those events arrive through input polling.
inline std::function<void()>& sdl_redraw_hook()
{
    static std::function<void()> hook;
    return hook;
}

// Set by the SDL display while open: saves what the simulated panel shows as a BMP.
// For scripted checks of quantization, ghosting, and A2 artifacts.
inline std::function<bool(const char* path)>& sdl_snapshot_hook()
{
    static std::function<bool(const char*)> hook;
    return hook;
}

} // namespace sumi
