// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "LivecodePanel.hpp"

struct LivecodeKeys : Module {
    enum { VELOCITY, PARAMS };
    enum { PITCH, GATE, VEL, OUTPUTS };
    static constexpr int keyCount = 24;
    static constexpr int channelCount = 16;
    int octave = 4;

    struct Voice {
        int note = -1;
        float pitch = 0.f;
        float velocity = 0.f;
        float minimumGate = 0.f;
        bool held = false;
    };
    struct Note {
        int midi;
        bool on;
    };
    dsp::RingBuffer<Note, 128> events;
    std::array<Voice, channelCount> voices {};
    int channels = 1;

    LivecodeKeys() {
        config(PARAMS, 0, OUTPUTS, 0);
        configParam(VELOCITY, 1.f, 127.f, 100.f, "Velocity")->snapEnabled = true;
        configOutput(PITCH, "Polyphonic pitch (1 V/oct)");
        configOutput(GATE, "Polyphonic gate");
        configOutput(VEL, "Polyphonic velocity");
    }

    void note(int midi, bool on) {
        if (events.full()) {
            events.clear();
            events.push({-1, false});
        }
        events.push({midi, on});
    }

    void process(const ProcessArgs& args) override {
        while (!events.empty()) {
            const auto event = events.shift();
            if (event.midi < 0) {
                for (auto& voice : voices)
                    voice.held = false;
                continue;
            }
            auto voice = std::find_if(voices.begin(), voices.end(), [&](const Voice& v) {
                return v.note == event.midi;
            });
            if (event.on && voice == voices.end())
                voice = std::find_if(voices.begin(), voices.end(), [](const Voice& v) { return v.note < 0; });
            if (voice == voices.end())
                continue;
            voice->held = event.on;
            if (event.on) {
                voice->note = event.midi;
                voice->minimumGate = .015f;
                voice->pitch = (event.midi - 60) / 12.f;
                voice->velocity = params[VELOCITY].getValue() * (10.f / 127.f);
                channels = std::max(channels, int(voice - voices.begin()) + 1);
            }
        }

        for (int i = 0; i < channels; ++i) {
            auto& voice = voices[i];
            const bool gate = voice.held || voice.minimumGate > 0.f;
            if (!gate)
                voice.note = -1;
            voice.minimumGate = std::max(0.f, voice.minimumGate - args.sampleTime);
            outputs[PITCH].setVoltage(voice.pitch, i);
            outputs[GATE].setVoltage(gate ? 10.f : 0.f, i);
            outputs[VEL].setVoltage(voice.velocity, i);
        }
        for (auto& output : outputs)
            output.setChannels(channels);
    }

    void onBypass(const BypassEvent& e) override {
        Module::onBypass(e);
        events.clear();
        for (auto& voice : voices)
            voice = Voice{};
        for (auto& output : outputs)
            output.clearVoltages();
        channels = 1;
    }

    json_t* dataToJson() override {
        auto* data = json_object();
        json_object_set_new(data, "octave", json_integer(octave));
        return data;
    }

    void dataFromJson(json_t* data) override {
        if (auto* value = json_object_get(data, "octave"))
            octave = math::clamp(int(json_integer_value(value)), 0, 7);
    }
};

struct LivecodeKeybed : widget::OpaqueWidget {
    static constexpr int count = LivecodeKeys::keyCount;
    static constexpr float whiteWidth = 21.f;
    static constexpr float height = 144.f;
    static constexpr float blackWidth = 13.f;
    static constexpr float blackHeight = 88.f;
    const char* const keyMap = "zsxdcvgbhnjmq2w3er5t6y7u";
    LivecodeKeys* keys;
    std::array<bool, count> held {};
    int pointer = -1;
    Vec pointerPos;
    bool focused = false;

    explicit LivecodeKeybed(LivecodeKeys* module) : keys(module) {}

    static bool black(int key) {
        const int pitch = key % 12;
        return pitch == 1 || pitch == 3 || pitch == 6 || pitch == 8 || pitch == 10;
    }

    float keyX(int key) const {
        int whites = 0;
        for (int i = 0; i < key; ++i)
            whites += !black(i);
        return whites * whiteWidth - (black(key) ? blackWidth * .5f : 0.f);
    }

    int keyAt(Vec pos) const {
        for (bool checkBlack : {true, false}) {
            for (int i = 0; i < count; ++i) {
                if (black(i) != checkBlack)
                    continue;
                const Vec size(checkBlack ? blackWidth : whiteWidth, checkBlack ? blackHeight : height);
                if (Rect(Vec(keyX(i), 0), size).contains(pos))
                    return i;
            }
        }
        return -1;
    }

    void send(int key, bool on) {
        if (keys && !keys->isBypassed())
            keys->note(12 * (keys->octave + 1) + key, on);
    }

    void release() {
        if (keys && (pointer >= 0 || std::any_of(held.begin(), held.end(), [](bool value) { return value; })))
            keys->note(-1, false);
        held.fill(false);
        pointer = -1;
    }

    void updatePointer(int key) {
        if (pointer == key)
            return;
        if (pointer >= 0 && !held[pointer])
            send(pointer, false);
        pointer = key;
        if (pointer >= 0 && !held[pointer])
            send(pointer, true);
    }

    void onButton(const ButtonEvent& e) override {
        if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_PRESS) {
            e.consume(this);
            pointerPos = e.pos;
            updatePointer(keyAt(pointerPos));
        } else {
            OpaqueWidget::onButton(e);
        }
    }

    void onDragEnd(const DragEndEvent& e) override {
        if (e.button == GLFW_MOUSE_BUTTON_LEFT)
            updatePointer(-1);
    }

    void onDragMove(const DragMoveEvent& e) override {
        if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
            pointerPos = pointerPos.plus(e.mouseDelta.div(getAbsoluteZoom()));
            updatePointer(keyAt(pointerPos));
        }
    }

    void onSelect(const SelectEvent&) override { focused = true; }
    void onDeselect(const DeselectEvent&) override {
        focused = false;
        release();
    }

    void onSelectKey(const SelectKeyEvent& e) override {
        if (e.action != GLFW_RELEASE && (e.mods & RACK_MOD_MASK))
            return;
        for (int i = 0; i < count; ++i) {
            if (e.key != keyMap[i] && e.key != std::toupper(static_cast<unsigned char>(keyMap[i])))
                continue;
            e.consume(this);
            if (e.action == GLFW_PRESS && !held[i]) {
                held[i] = true;
                if (pointer != i)
                    send(i, true);
            } else if (e.action == GLFW_RELEASE && held[i]) {
                held[i] = false;
                if (pointer != i)
                    send(i, false);
            }
            return;
        }
    }

    void step() override {
        if (keys && (keys->isBypassed() || (focused && !EM_ASM_INT({
            return document.hasFocus() && !document.hidden;
        }))))
            release();
        OpaqueWidget::step();
    }

    void draw(const DrawArgs& args) override {
        auto* vg = args.vg;
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgFontSize(vg, 10);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        for (bool drawBlack : {false, true}) {
            for (int i = 0; i < count; ++i) {
                if (black(i) != drawBlack)
                    continue;
                const float w = drawBlack ? blackWidth : whiteWidth;
                const float h = drawBlack ? blackHeight : height;
                const float x = keyX(i);
                nvgBeginPath(vg);
                nvgRoundedRect(vg, x + .5f, 0, w - 1.f, h, 2);
                nvgFillColor(vg, held[i] || pointer == i ? nvgRGB(212, 87, 105)
                    : drawBlack ? nvgRGB(36, 37, 38) : nvgRGB(185, 186, 186));
                nvgFill(vg);
                const char label[] = {keyMap[i], '\0'};
                nvgFillColor(vg, drawBlack ? nvgRGB(211, 212, 212) : nvgRGB(39, 40, 40));
                nvgText(vg, x + w * .5f, h - 15.f, label, nullptr);
            }
        }
    }
};

struct LivecodeKeysWidget : ModuleWidgetWithSideScrews<22> {
    LivecodeKeys* keys;
    LivecodeKeybed* keybed;

    struct OctaveButton : ui::Button {
        LivecodeKeysWidget* owner {};
        int delta = 0;
        void onAction(const ActionEvent&) override {
            if (!owner->keys)
                return;
            owner->keybed->release();
            owner->keys->octave = math::clamp(owner->keys->octave + delta, 0, 7);
            APP->event->setSelectedWidget(owner->keybed);
        }
    };

    explicit LivecodeKeysWidget(LivecodeKeys* module) : keys(module) {
        setModule(module);
        box.size = Vec(22 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT);
        createAndAddScrews();
        keybed = new LivecodeKeybed(module);
        keybed->box = Rect(Vec(18, 125), Vec(14 * LivecodeKeybed::whiteWidth, LivecodeKeybed::height));
        addChild(keybed);
        for (int delta : {-1, 1}) {
            auto* button = new OctaveButton;
            button->owner = this;
            button->delta = delta;
            button->text = delta < 0 ? "−" : "+";
            button->box = Rect(Vec(delta < 0 ? 18 : 85, 84), Vec(25, 20));
            addChild(button);
        }
        addParam(createParamCentered<RoundBlackKnob>(Vec(287, 92), module, LivecodeKeys::VELOCITY));
        for (int i = 0; i < LivecodeKeys::OUTPUTS; ++i)
            addOutput(createOutputCentered<PJ301MPort>(Vec(190 + i * 49, 326), module, i));
    }

    void draw(const DrawArgs& args) override {
        auto* vg = args.vg;
        drawLivecodePanel(vg, box.size);
        const auto label = settings::preferDarkPanels ? nvgRGB(174, 176, 178) : nvgRGB(62, 62, 62);
        nvgFontFaceId(vg, APP->window->uiFont->handle);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, label);
        nvgFontSize(vg, 15);
        nvgText(vg, 165, 41, "Keys", nullptr);
        nvgFontSize(vg, 10);
        nvgText(vg, 64, 70, "OCTAVE", nullptr);
        nvgText(vg, 287, 70, "VELOCITY", nullptr);
        nvgText(vg, 239, 291, "OUT", nullptr);
        const char* names[] = {"PITCH", "GATE", "VEL"};
        for (int i = 0; i < LivecodeKeys::OUTPUTS; ++i) {
            const float x = 190 + i * 49;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x - 21, 303, 42, 37, 4);
            nvgFillColor(vg, nvgRGB(170, 171, 172));
            nvgFill(vg);
            nvgFillColor(vg, nvgRGB(22, 22, 23));
            nvgText(vg, x, 310, names[i], nullptr);
        }
        char octave[8];
        std::snprintf(octave, sizeof(octave), "C%d", keys ? keys->octave : 4);
        nvgFillColor(vg, label);
        nvgFontSize(vg, 14);
        nvgText(vg, 64, 94, octave, nullptr);
        if (keybed->focused) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, 17, 124, keybed->box.size.x + 2, keybed->box.size.y + 2, 3);
            nvgStrokeColor(vg, nvgRGB(190, 92, 104));
            nvgStroke(vg);
        }
        ModuleWidget::draw(args);
    }
};

Model* modelLivecodeKeys = createModel<LivecodeKeys, LivecodeKeysWidget>("Keys");
