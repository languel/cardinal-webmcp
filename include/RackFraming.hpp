// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <app/Scene.hpp>
#include <context.hpp>

namespace rack {
namespace app {

inline bool frameRackBounds(const math::Rect& bounds) {
    auto* scroll = APP->scene->rackScroll;
    const auto size = scroll->getSize();
    if (!bounds.size.isFinite() || bounds.size.x <= 0.f || bounds.size.y <= 0.f
        || size.x <= 40.f || size.y <= 40.f)
        return false;

    const auto available = size - math::Vec(40.f, 40.f);
    scroll->setZoom(std::min(available.x / bounds.size.x, available.y / bounds.size.y));
    const auto centerOffset = bounds.getCenter() - RACK_OFFSET - size / (2.f * scroll->getZoom());
    scroll->setGridOffset(centerOffset / RACK_GRID_SIZE);
    return true;
}

inline bool frameAllModules() {
    auto* modules = APP->scene->rack->getModuleContainer();
    return !modules->children.empty() && frameRackBounds(modules->getChildrenBoundingBox());
}

inline bool frameSelectedModules(bool preferPointer) {
    auto* scene = APP->scene;
    auto* rack = scene->rack;
    if (preferPointer && scene->rackScroll->box.contains(scene->getMousePos())) {
        const auto mouse = rack->getMousePos();
        for (auto* module : rack->getModules()) {
            if (module->box.contains(mouse))
                return frameRackBounds(module->box);
        }
    }

    const auto& selected = rack->getSelected();
    if (selected.empty())
        return false;
    auto bounds = (*selected.begin())->box;
    for (auto* module : selected)
        bounds = bounds.expand(module->box);
    return frameRackBounds(bounds);
}

} // namespace app
} // namespace rack
