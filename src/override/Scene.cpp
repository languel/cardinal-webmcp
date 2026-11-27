/*
 * DISTRHO Cardinal Plugin
 * Copyright (C) 2021-2025 Filipe Coelho <falktx@falktx.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * This file is an edited version of VCVRack's app/Scene.cpp
 * Copyright (C) 2016-2023 VCV.
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 */

#include <app/Scene.hpp>
#include <app/Browser.hpp>
#include <app/TipWindow.hpp>
#include <app/MenuBar.hpp>
#include <ui/MenuOverlay.hpp>
#include <context.hpp>
#include <engine/Engine.hpp>
#include <system.hpp>
#include <network.hpp>
#include <history.hpp>
#include <settings.hpp>
#include <patch.hpp>
#include <asset.hpp>
#include <RackFraming.hpp>
#include <RackNavigation.hpp>
#include <BrowserTransport.hpp>

#ifdef NDEBUG
# undef DEBUG
#endif

#include "../CardinalCommon.hpp"
#include "../CardinalRemote.hpp"

#include <algorithm>

namespace rack {
namespace app {

bool autoHideMenuBar = false;

bool handleRoomBrightnessKey(int key, int mods, int action) {
	const int modifiers = mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER | GLFW_MOD_ALT);
	const bool dimmer = key == GLFW_KEY_COMMA || key == '<';
	const bool brighter = key == GLFW_KEY_PERIOD || key == '>';
	if ((modifiers != GLFW_MOD_SUPER && modifiers != GLFW_MOD_CONTROL)
	    || (!dimmer && !brighter) || !(mods & GLFW_MOD_SHIFT))
		return false;
	if (action != GLFW_RELEASE) {
		const float delta = dimmer ? -.1f : .1f;
		settings::rackBrightness = math::clamp(settings::rackBrightness + delta, 0.f, 1.f);
	}
	return true;
}


struct ResizeHandle : widget::OpaqueWidget {
	math::Vec size;

	void draw(const DrawArgs& args) override {
		nvgStrokeColor(args.vg, nvgRGBf(1, 1, 1));
		nvgStrokeWidth(args.vg, 1);

		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, box.size.x, 0);
		nvgLineTo(args.vg, 0, box.size.y);
		nvgStroke(args.vg);

		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, box.size.x + 5, 0);
		nvgLineTo(args.vg, 0, box.size.y + 5);
		nvgStroke(args.vg);

		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, box.size.x + 10, 0);
		nvgLineTo(args.vg, 0, box.size.y + 10);
		nvgStroke(args.vg);

		nvgStrokeColor(args.vg, nvgRGBf(0, 0, 0));

		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, box.size.x+1, 0);
		nvgLineTo(args.vg, 0, box.size.y+1);
		nvgStroke(args.vg);

		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, box.size.x + 6, 0);
		nvgLineTo(args.vg, 0, box.size.y + 6);
		nvgStroke(args.vg);

		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, box.size.x + 11, 0);
		nvgLineTo(args.vg, 0, box.size.y + 11);
		nvgStroke(args.vg);
	}

	void onHover(const HoverEvent& e) override {
		e.consume(this);
	}

	void onEnter(const EnterEvent& e) override {
		glfwSetCursor(APP->window->win, glfwCreateStandardCursor(GLFW_RESIZE_NWSE_CURSOR));
	}

	void onLeave(const LeaveEvent& e) override {
		glfwSetCursor(APP->window->win, nullptr);
	}

	void onDragStart(const DragStartEvent&) override {
		size = APP->window->getSize();
	}

	void onDragMove(const DragMoveEvent& e) override {
		size = size.plus(e.mouseDelta.mult(APP->window->pixelRatio));
		APP->window->setSize(size.round());
	}
};


struct PerformanceRackScroll : RackScrollWidget {
	int64_t soloId = -1;
#ifdef __EMSCRIPTEN__
	double lastNavigation = -1.0;
	float lastZoom = 0.f;
	math::Vec lastOffset;
#endif

	math::Rect moduleBounds(ModuleWidget* module) {
		return math::Rect::fromMinMax(module->getRelativeOffset(math::Vec(), this),
			module->getRelativeOffset(module->box.size, this));
	}

	void draw(const DrawArgs& args) override {
		RackScrollWidget::draw(args);
		auto* module = soloId >= 0 ? rackWidget->getModule(soloId) : nullptr;
		if (!module)
			return;

		// Draw over the rest of the rack after its module and cable layers.
		const auto bounds = moduleBounds(module);
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
		nvgRect(args.vg, bounds.pos.x, bounds.pos.y, bounds.size.x, bounds.size.y);
		nvgPathWinding(args.vg, NVG_HOLE);
		nvgFillColor(args.vg, nvgRGB(8, 8, 8));
		nvgFill(args.vg);
	}

	void onButton(const ButtonEvent& e) override {
		auto* module = soloId >= 0 ? rackWidget->getModule(soloId) : nullptr;
		if (module && !moduleBounds(module).contains(e.pos)) {
			e.consume(this);
			return;
		}
		RackScrollWidget::onButton(e);
	}
};

#ifdef __EMSCRIPTEN__
RackNavigation rackNavigation() {
	if (!APP || !APP->scene || !APP->scene->rackScroll)
		return RackNavigation::Idle;
	if (APP->scene->browser && APP->scene->browser->isVisible())
		return RackNavigation::Moving;
	const auto* scroll = static_cast<PerformanceRackScroll*>(APP->scene->rackScroll);
	if (scroll->lastNavigation < 0.0)
		return RackNavigation::Idle;
	const double elapsed = system::getTime() - scroll->lastNavigation;
	if (elapsed < .16)
		return RackNavigation::Moving;
	return elapsed < .42 ? RackNavigation::Settling : RackNavigation::Idle;
}
#endif

struct Scene::Internal {
	ResizeHandle* resizeHandle = nullptr;
	float previousZoom = 1.f;
	math::Vec previousOffset;
	bool requestedFullscreen = false;
	bool enteredFullscreen = false;

	void leaveSolo(PerformanceRackScroll* scroll) {
		scroll->soloId = -1;
		scroll->setZoom(previousZoom);
		scroll->setGridOffset(previousOffset);
		if (requestedFullscreen && APP->window->isFullScreen())
			APP->window->setFullScreen(false);
		requestedFullscreen = enteredFullscreen = false;
	}

	bool heldArrowKeys[4] = {};

	double lastSceneChangeTime = 0.0;
	int historyActionIndex = -1;
};


Scene::Scene() {
	internal = new Internal;

	rackScroll = new PerformanceRackScroll;
	addChild(rackScroll);

	rack = rackScroll->rackWidget;

	menuBar = createMenuBar();
	addChild(menuBar);

	browser = browserCreate();
	browser->hide();
	addChild(browser);

	if (isStandalone() || isMini())
		return;

	internal->resizeHandle = new ResizeHandle;
	internal->resizeHandle->box.size = math::Vec(16, 16);
	addChild(internal->resizeHandle);
}


Scene::~Scene() {
	delete internal;
}


math::Vec Scene::getMousePos() {
	return mousePos;
}


void Scene::step() {
	auto* scroll = static_cast<PerformanceRackScroll*>(rackScroll);
	auto* solo = scroll->soloId >= 0 ? rack->getModule(scroll->soloId) : nullptr;
	const bool fullscreen = APP->window->isFullScreen();
	if (scroll->soloId >= 0) {
		if (!solo || (internal->enteredFullscreen && !fullscreen)) {
			internal->leaveSolo(scroll);
			solo = nullptr;
		}
		else if (internal->requestedFullscreen && fullscreen) {
			internal->enteredFullscreen = true;
		}
	}
	if (solo) {
		menuBar->hide();
		rackScroll->box.pos.y = 0;
	}
	else if (fullscreen || autoHideMenuBar) {
		if (autoHideMenuBar) {
			const bool menuOpen = menuBar->isVisible() && std::any_of(children.begin(), children.end(),
				[this](widget::Widget* child) {
					return child != browser && child->isVisible() && dynamic_cast<ui::MenuOverlay*>(child);
				});
			menuBar->setVisible(mousePos.y < menuBar->box.size.y || menuOpen);
		}
		// Expand RackScrollWidget to cover entire screen if fullscreen
		rackScroll->box.pos.y = 0;
	}
	else {
		// Always show MenuBar if not fullscreen
		menuBar->show();
		rackScroll->box.pos.y = menuBar->box.size.y;
	}

	if (internal->resizeHandle != nullptr)
		internal->resizeHandle->box.pos = box.size.minus(internal->resizeHandle->box.size);

	// Resize owned descendants
	menuBar->box.size.x = box.size.x;
	rackScroll->box.size = box.size.minus(rackScroll->box.pos);
	if (solo)
		frameRackBounds(solo->box);

	// Scroll RackScrollWidget with arrow keys
	math::Vec arrowDelta;
	if (internal->heldArrowKeys[0]) {
		arrowDelta.x -= 1;
	}
	if (internal->heldArrowKeys[1]) {
		arrowDelta.x += 1;
	}
	if (internal->heldArrowKeys[2]) {
		arrowDelta.y -= 1;
	}
	if (internal->heldArrowKeys[3]) {
		arrowDelta.y += 1;
	}

	if (!arrowDelta.isZero()) {
		int mods = APP->window->getMods();
		float arrowSpeed = 32.f;
		if ((mods & RACK_MOD_MASK) == RACK_MOD_CTRL)
			arrowSpeed /= 4.f;
		if ((mods & RACK_MOD_MASK) == GLFW_MOD_SHIFT)
			arrowSpeed *= 4.f;
		if ((mods & RACK_MOD_MASK) == (RACK_MOD_CTRL | GLFW_MOD_SHIFT))
			arrowSpeed /= 16.f;

		rackScroll->offset += arrowDelta * arrowSpeed;
	}

#ifdef __EMSCRIPTEN__
	const float zoom = scroll->getZoom();
	const auto offset = scroll->getGridOffset();
	if (scroll->lastZoom > 0.f && (std::abs(zoom - scroll->lastZoom) > .0001f
	    || (offset - scroll->lastOffset).square() > .000001f))
		scroll->lastNavigation = system::getTime();
	scroll->lastZoom = zoom;
	scroll->lastOffset = offset;
#endif

	if (remoteUtils::RemoteDetails* const remoteDetails = remoteUtils::getRemote()) {
		idleRemote(remoteDetails);

		if (remoteDetails->autoDeploy && remoteDetails->connected) {
			const int actionIndex = APP->history->actionIndex;
			const double time = system::getTime();

			if (internal->historyActionIndex == -1) {
				internal->historyActionIndex = actionIndex;
				internal->lastSceneChangeTime = time;
			} else if (remoteDetails->first ||
				(internal->historyActionIndex != actionIndex
				&& actionIndex > 0
				&& time - internal->lastSceneChangeTime >= 1.0)) {
				if (remoteDetails->first) {
					remoteDetails->first = false;
					remoteUtils::sendFullPatchToRemote(remoteDetails);
				} else {
					const std::string& name(APP->history->actions[actionIndex - 1]->name);
					static const std::vector<std::string> ignoredNames = {
						"move knob",
						"move modules",
						"move switch",
					};
					if (std::find(ignoredNames.cbegin(), ignoredNames.cend(), name) == ignoredNames.cend()) {
						d_debug("action '%s'\n", APP->history->actions[actionIndex - 1]->name.c_str());
						remoteUtils::sendFullPatchToRemote(remoteDetails);

						if (remoteDetails->screenshot) {
							window::generateScreenshot();
						}
					}
				}
				internal->historyActionIndex = actionIndex;
				internal->lastSceneChangeTime = time;
			}
		}
	}

	Widget::step();
}


void Scene::draw(const DrawArgs& args) {
	Widget::draw(args);
}


void Scene::onHover(const HoverEvent& e) {
	mousePos = e.pos;
	if (static_cast<PerformanceRackScroll*>(rackScroll)->soloId < 0
	    && !autoHideMenuBar && mousePos.y < menuBar->box.size.y) {
		menuBar->show();
	}
	OpaqueWidget::onHover(e);
}


void Scene::onDragHover(const DragHoverEvent& e) {
	mousePos = e.pos;
	OpaqueWidget::onDragHover(e);
}


void Scene::onHoverKey(const HoverKeyEvent& e) {
#ifdef __EMSCRIPTEN__
	if (e.action == GLFW_PRESS && e.key == GLFW_KEY_SPACE && (e.mods & RACK_MOD_MASK) == GLFW_MOD_SHIFT) {
		auto& transport = browserTransport();
		transport.toggle();
		e.consume(this);
		return;
	}
#endif

	auto* scroll = static_cast<PerformanceRackScroll*>(rackScroll);
	const bool soloShortcut = e.key == GLFW_KEY_F && (e.mods & RACK_MOD_MASK) == GLFW_MOD_SHIFT;
	if (e.action == GLFW_PRESS && scroll->soloId >= 0 && (soloShortcut || e.key == GLFW_KEY_ESCAPE)) {
		internal->leaveSolo(scroll);
		e.consume(this);
		return;
	}
	if (e.action == GLFW_PRESS && soloShortcut) {
		auto* module = moduleUnderPointer();
		const auto& selected = rack->getSelected();
		if (!module && selected.size() == 1)
			module = *selected.begin();
		if (module && module->getModule()) {
			internal->previousZoom = scroll->getZoom();
			internal->previousOffset = scroll->getGridOffset();
			scroll->soloId = module->getModule()->id;
			internal->requestedFullscreen = !APP->window->isFullScreen();
			if (internal->requestedFullscreen)
				APP->window->setFullScreen(true);
			e.consume(this);
			return;
		}
	}
	if (handleRoomBrightnessKey(e.key, e.mods, e.action)) {
		e.consume(this);
		return;
	}
	if (e.action == GLFW_PRESS && (e.mods & RACK_MOD_MASK) == 0) {
		if (((e.key == GLFW_KEY_F || e.keyName == "f") && frameAllModules())
		    || (e.key == GLFW_KEY_PERIOD && frameSelectedModules(true))) {
			e.consume(this);
			return;
		}
	}

	// Key commands that override children
	if (e.action == GLFW_PRESS || e.action == GLFW_REPEAT) {
		// DEBUG("key '%d '%c' scancode %d '%c' keyName '%s'", e.key, e.key, e.scancode, e.scancode, e.keyName.c_str());
		if (e.keyName == "n" && (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL) {
			patchUtils::loadTemplateDialog(false);
			e.consume(this);
		}
		if (e.keyName == "q" && (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL) {
			APP->window->close();
			e.consume(this);
		}
		if (e.keyName == "o" && (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL) {
			patchUtils::loadDialog();
			e.consume(this);
		}
		if (e.keyName == "o" && (e.mods & RACK_MOD_MASK) == (RACK_MOD_CTRL | GLFW_MOD_SHIFT)) {
			patchUtils::revertDialog();
			e.consume(this);
		}
		if (e.keyName == "s" && (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL) {
			// NOTE: for plugin versions it will do nothing if path is empty, intentionally
			if (APP->patch->path.empty()) {
				if (isStandalone())
					patchUtils::saveAsDialog();
			} else {
				patchUtils::saveDialog(APP->patch->path);
			}
			e.consume(this);
		}
		if (e.keyName == "s" && (e.mods & RACK_MOD_MASK) == (RACK_MOD_CTRL | GLFW_MOD_SHIFT)) {
			patchUtils::saveAsDialog();
			e.consume(this);
		}
		if (e.keyName == "z" && (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL) {
			APP->history->undo();
			e.consume(this);
		}
		if (e.keyName == "z" && (e.mods & RACK_MOD_MASK) == (RACK_MOD_CTRL | GLFW_MOD_SHIFT)) {
			APP->history->redo();
			e.consume(this);
		}
		if (e.keyName == "-" && (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL) {
			float zoom = std::log2(APP->scene->rackScroll->getZoom());
			zoom *= 2;
			zoom = std::ceil(zoom - 0.01f) - 1;
			zoom /= 2;
			APP->scene->rackScroll->setZoom(std::pow(2.f, zoom));
			e.consume(this);
		}
		// Numpad has a "+" key, but the main keyboard section hides it under "="
		if ((e.keyName == "=" || e.keyName == "+") && (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL) {
			float zoom = std::log2(APP->scene->rackScroll->getZoom());
			zoom *= 2;
			zoom = std::floor(zoom + 0.01f) + 1;
			zoom /= 2;
			APP->scene->rackScroll->setZoom(std::pow(2.f, zoom));
			e.consume(this);
		}
		if ((e.keyName == "0" || e.keyName == "1") && (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL) {
			APP->scene->rackScroll->setZoom(1.f);
			e.consume(this);
		}
		if (e.keyName == "2" && (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL) {
			APP->scene->rackScroll->setZoom(2.f);
			e.consume(this);
		}
		if (e.key == GLFW_KEY_F1 && (e.mods & RACK_MOD_MASK) == 0) {
			patchUtils::openBrowser("https://vcvrack.com/manual/");
			e.consume(this);
		}
		if (e.key == GLFW_KEY_F3 && (e.mods & RACK_MOD_MASK) == 0) {
			settings::cpuMeter ^= true;
			e.consume(this);
		}
		if (e.key == GLFW_KEY_F7 && (e.mods & RACK_MOD_MASK) == 0) {
			if (remoteUtils::RemoteDetails* const remoteDetails = remoteUtils::getRemote())
			{
				if (remoteDetails->connected) {
					remoteUtils::sendFullPatchToRemote(remoteDetails);

					if (remoteDetails->screenshot) {
						window::generateScreenshot();
					}
				}
			}
			e.consume(this);
		}
		if (e.key == GLFW_KEY_F9 && (e.mods & RACK_MOD_MASK) == 0) {
			window::generateScreenshot();
			e.consume(this);
		}
#ifdef DISTRHO_OS_WASM
		if (e.key == GLFW_KEY_F11 && (e.mods & RACK_MOD_MASK) == 0) {
			APP->window->setFullScreen(!APP->window->isFullScreen());
			// The MenuBar will be hidden when the mouse moves over the RackScrollWidget.
			// menuBar->hide();
			e.consume(this);
		}
#endif

		// Module selections
		if (e.keyName == "a" && (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL) {
			rack->selectAll();
			e.consume(this);
		}
		if (e.keyName == "a" && (e.mods & RACK_MOD_MASK) == (RACK_MOD_CTRL | GLFW_MOD_SHIFT)) {
			rack->deselectAll();
			e.consume(this);
		}
		if (e.keyName == "c" && (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL) {
			if (rack->hasSelection()) {
				rack->copyClipboardSelection();
				e.consume(this);
			}
		}
		if (e.keyName == "i" && (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL) {
			if (rack->hasSelection()) {
				rack->resetSelectionAction();
				e.consume(this);
			}
		}
		if (e.keyName == "r" && (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL) {
			if (rack->hasSelection()) {
				rack->randomizeSelectionAction();
				e.consume(this);
			}
		}
		if (e.keyName == "u" && (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL) {
			if (rack->hasSelection()) {
				rack->disconnectSelectionAction();
				e.consume(this);
			}
		}
		if (e.keyName == "e" && (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL) {
			if (rack->hasSelection()) {
				rack->bypassSelectionAction(!rack->isSelectionBypassed());
				e.consume(this);
			}
		}
		if (e.keyName == "d" && (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL) {
			if (rack->hasSelection()) {
				rack->cloneSelectionAction(false);
				e.consume(this);
			}
		}
		if (e.keyName == "d" && (e.mods & RACK_MOD_MASK) == (RACK_MOD_CTRL | GLFW_MOD_SHIFT)) {
			if (rack->hasSelection()) {
				rack->cloneSelectionAction(true);
				e.consume(this);
			}
		}
		if ((e.key == GLFW_KEY_DELETE || e.key == GLFW_KEY_BACKSPACE) && (e.mods & RACK_MOD_MASK) == 0) {
			if (rack->hasSelection()) {
				rack->deleteSelectionAction();
				e.consume(this);
			}
		}
	}

	// Scroll RackScrollWidget with arrow keys
	if (e.action == GLFW_PRESS || e.action == GLFW_RELEASE) {
		if (e.key == GLFW_KEY_LEFT) {
			internal->heldArrowKeys[0] = (e.action == GLFW_PRESS);
			e.consume(this);
		}
		if (e.key == GLFW_KEY_RIGHT) {
			internal->heldArrowKeys[1] = (e.action == GLFW_PRESS);
			e.consume(this);
		}
		if (e.key == GLFW_KEY_UP) {
			internal->heldArrowKeys[2] = (e.action == GLFW_PRESS);
			e.consume(this);
		}
		if (e.key == GLFW_KEY_DOWN) {
			internal->heldArrowKeys[3] = (e.action == GLFW_PRESS);
			e.consume(this);
		}
	}

	if (e.isConsumed())
		return;
	OpaqueWidget::onHoverKey(e);
	if (e.isConsumed())
		return;

	// Key commands that can be overridden by children
	if (e.action == GLFW_PRESS || e.action == GLFW_REPEAT) {
		if (e.keyName == "v" && (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL) {
			rack->pasteClipboardAction();
			e.consume(this);
		}
		if ((e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER) && (e.mods & RACK_MOD_MASK) == 0) {
			browser->show();
			e.consume(this);
		}
	}
}


void Scene::onPathDrop(const PathDropEvent& e) {
	if (e.paths.size() >= 1) {
		const std::string& path = e.paths[0];
		std::string extension = system::getExtension(path);

		if (extension == ".vcv") {
			patchUtils::loadPathDialog(path);
			e.consume(this);
			return;
		}
		if (extension == ".vcvs") {
			APP->scene->rack->loadSelection(path);
			e.consume(this);
			return;
		}
	}

	OpaqueWidget::onPathDrop(e);
}


} // namespace app
} // namespace rack
