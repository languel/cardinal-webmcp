/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <GLFW/glfw3.h>
#include <ui/Slider.hpp>

namespace rack {
namespace ui {

struct AbsoluteSlider : Slider {
	void onButton(const ButtonEvent& e) override {
		if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_PRESS) {
			pointerX = e.pos.x;
			updateValue();
		}
		Slider::onButton(e);
	}

	void onDragStart(const DragStartEvent&) override {}

	void onDragMove(const DragMoveEvent& e) override {
		if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
			pointerX += e.mouseDelta.x;
			updateValue();
		}
	}

	void onDragEnd(const DragEndEvent&) override {}

private:
	float pointerX = 0.f;

	void updateValue() {
		if (quantity && box.size.x > 0.f)
			quantity->setScaledValue(math::clamp(pointerX / box.size.x, 0.f, 1.f));
	}
};

} // namespace ui
} // namespace rack
