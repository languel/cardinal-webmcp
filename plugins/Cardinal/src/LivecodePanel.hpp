// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ModuleWidgets.hpp"

inline void drawLivecodePanel(NVGcontext* vg, rack::Vec size) {
    const bool dark = rack::settings::preferDarkPanels;
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, size.x, size.y);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, size.y,
        dark ? nvgRGB(0x18, 0x19, 0x19) : nvgRGB(0xe7, 0xe6, 0xe6),
        dark ? nvgRGB(0x21, 0x22, 0x22) : nvgRGB(0xde, 0xdd, 0xdd)));
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, .75f, size.y);
    nvgRect(vg, size.x - .75f, 0, .75f, size.y);
    nvgFillColor(vg, dark ? nvgRGB(74, 75, 75) : nvgRGB(163, 163, 163));
    nvgFill(vg);
}
