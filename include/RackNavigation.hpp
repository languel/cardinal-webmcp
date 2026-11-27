// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace rack {
namespace app {

enum class RackNavigation { Idle, Moving, Settling };

#ifdef __EMSCRIPTEN__
RackNavigation rackNavigation();
#else
inline RackNavigation rackNavigation() { return RackNavigation::Idle; }
#endif

// Widgets that also produce a patch signal may need to render while moving.
struct NavigationSignal {
    virtual bool drawsPatchSignal() const = 0;
protected:
    ~NavigationSignal() = default;
};

} // namespace app
} // namespace rack
