// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "LivecodePanel.hpp"

struct BrowserTransportModule : HostTime {
    enum { TEMPO, PLAY, SYNC, TAP, PARAMS };
    enum { RUN_INPUT, SYNC_INPUT, TAP_INPUT, INPUTS };
    enum { PLAY_LIGHT = kHostTimeCount, SYNC_LIGHT, TAP_LIGHT, LIGHTS };

    dsp::SchmittTrigger runInput, syncInput, tapInput;
    dsp::BooleanTrigger playButton, syncButton, tapButton;
    bool panelPress[PARAMS] {};
    float lastTempo = 0.f;

    BrowserTransportModule() : HostTime(PARAMS, INPUTS, LIGHTS) {
        configParam(TEMPO, 20.f, 400.f, 120.f, "Tempo", " BPM")->snapEnabled = true;
        configButton(PLAY, "Play / stop");
        configButton(SYNC, "Return to bar 1");
        configButton(TAP, "Tap tempo");
        configInput(RUN_INPUT, "Play / stop toggle");
        configInput(SYNC_INPUT, "Return to bar 1");
        configInput(TAP_INPUT, "Tap tempo (one pulse per beat)");
        const char* names[] = {"Playing", "Reset", "Bar", "Beat", "Step", "Bar phase", "Beat phase", "BPM (V/oct)"};
        for (int i = 0; i < kHostTimeCount; ++i)
            configOutput(i, names[i]);
    }

    void processTerminalInput(const ProcessArgs& args) override {
        auto& clock = browserTransport();
        if (!isBypassed()) {
            const float tempo = params[TEMPO].getValue();
            if (lastTempo > 0.f && tempo != lastTempo)
                clock.setBpm(tempo);

            const bool play = playButton.process(params[PLAY].getValue() > .5f) || panelPress[PLAY];
            const bool sync = syncButton.process(params[SYNC].getValue() > .5f) || panelPress[SYNC];
            const bool tap = tapButton.process(params[TAP].getValue() > .5f) || panelPress[TAP];
            panelPress[PLAY] = panelPress[SYNC] = panelPress[TAP] = false;
            if (runInput.process(inputs[RUN_INPUT].getVoltage(), .1f, 1.f) || play)
                clock.toggle();
            if (syncInput.process(inputs[SYNC_INPUT].getVoltage(), .1f, 1.f) || sync)
                clock.rewind();
            if (tapInput.process(inputs[TAP_INPUT].getVoltage(), .1f, 1.f) || tap)
                clock.tap(args.frame * static_cast<double>(args.sampleTime));

            lastTempo = clock.bpm;
            if (tempo != lastTempo)
                params[TEMPO].setValue(lastTempo);
            lights[PLAY_LIGHT].setBrightness(clock.playing);
            lights[SYNC_LIGHT].setBrightnessSmooth(sync || pcontext->reset, args.sampleTime * .25f);
            lights[TAP_LIGHT].setBrightnessSmooth(tap || params[TAP].getValue() > .5f || tapInput.isHigh(), args.sampleTime);
        }
        HostTime::processTerminalInput(args);
        if (!isBypassed())
            outputs[kHostTimeBPM].setVoltage(std::log2(clock.bpm / 120.f));
    }
};

struct BrowserTransportWidget : ModuleWidgetWithSideScrews<14> {
    using Clock = BrowserTransport;
    static constexpr float width = 210.f;
    const std::string monoFontPath = asset::system("res/fonts/ShareTechMono-Regular.ttf");

    static float controlX(int column) { return 26.f + 35.f * column; }

    using Button = VCVLightButton<MediumLight<RedLight>>;

    template<int id>
    struct TransportButton : Button {
        void onDragStart(const DragStartEvent& e) override {
            if (e.button == GLFW_MOUSE_BUTTON_LEFT && module && !module->isBypassed()) {
                auto& clock = browserTransport();
                if (id == BrowserTransportModule::PLAY && !clock.enabled) {
                    // The first browser gesture can start audio after this press has ended.
                    ParamWidget::onDragStart(e);
                    clock.play();
                    return;
                }
                // Keep clicks shorter than an audio block until the engine can read them.
                static_cast<BrowserTransportModule*>(module)->panelPress[id] = true;
            }
            Button::onDragStart(e);
        }
    };

    struct Setting : ui::Button {
        bool meter = false;

        void step() override {
            const auto& clock = browserTransport();
            text = meter ? std::to_string(clock.beatsPerBar) + "/" + std::to_string(clock.beatType)
                : clock.quantizeBeats == Clock::NextBar ? "BAR"
                : clock.quantizeBeats == Clock::NextBeat ? "BEAT" : "OFF";
            ui::Button::step();
        }

        void onAction(const ActionEvent&) override {
            auto* menu = createMenu();
            if (meter) {
                menu->addChild(createSubmenuItem("Beats per bar", "", [](ui::Menu* submenu) {
                    for (int beats = 1; beats <= 16; ++beats)
                        submenu->addChild(createCheckMenuItem(std::to_string(beats), "",
                            [beats] { return browserTransport().beatsPerBar == beats; },
                            [beats] { browserTransport().setMeter(beats, browserTransport().beatType); }));
                }));
                menu->addChild(createSubmenuItem("Beat unit", "", [](ui::Menu* submenu) {
                    for (int unit : {2, 4, 8, 16})
                        submenu->addChild(createCheckMenuItem("1/" + std::to_string(unit), "",
                            [unit] { return browserTransport().beatType == unit; },
                            [unit] { browserTransport().setMeter(browserTransport().beatsPerBar, unit); }));
                }));
            } else {
                const char* labels[] = {"BEAT", "BAR", "OFF"};
                const int modes[] = {Clock::NextBeat, Clock::NextBar, Clock::Immediate};
                for (int i = 0; i < 3; ++i) {
                    const int mode = modes[i];
                    menu->addChild(createCheckMenuItem(labels[i], "",
                        [mode] { return browserTransport().quantizeBeats == mode; },
                        [mode] { browserTransport().quantizeBeats = mode; }));
                }
            }
        }
    };

    explicit BrowserTransportWidget(BrowserTransportModule* m) {
        using M = BrowserTransportModule;
        setModule(m);
        box.size = Vec(width, RACK_GRID_HEIGHT);
        createAndAddScrews();
        addParam(createParamCentered<RoundBlackKnob>(Vec(44, 141), m, M::TEMPO));
        for (bool meter : {true, false}) {
            auto* setting = new Setting;
            setting->meter = meter;
            setting->box = Rect(Vec(meter ? 73 : 126, 132), Vec(meter ? 46 : 72, 20));
            addChild(setting);
        }
        addParam(createLightParamCentered<TransportButton<M::PLAY>>(Vec(controlX(0), 191), m, M::PLAY, M::PLAY_LIGHT));
        addParam(createLightParamCentered<TransportButton<M::SYNC>>(Vec(controlX(1), 191), m, M::SYNC, M::SYNC_LIGHT));
        addParam(createLightParamCentered<TransportButton<M::TAP>>(Vec(controlX(2), 191), m, M::TAP, M::TAP_LIGHT));
        for (int i = 0; i < M::INPUTS; ++i)
            addInput(createInputCentered<PJ301MPort>(Vec(controlX(i), 225), m, i));
        for (int i = 0; i < M::kHostTimeCount; ++i)
            addOutput(createOutputCentered<PJ301MPort>(Vec(138 + 45 * (i % 2), 216 + 39 * (i / 2)), m, i));
    }

    void draw(const DrawArgs& args) override {
        auto* vg = args.vg;
        drawLivecodePanel(vg, box.size);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, 12, 40, width - 24, 64, 5);
        nvgFillColor(vg, nvgRGB(9, 9, 10));
        nvgFill(vg);
        const bool dark = settings::preferDarkPanels;
        const auto label = dark ? nvgRGB(174, 176, 178) : nvgRGB(62, 62, 62);
        nvgFontFaceId(vg, 0);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontSize(vg, 15);
        nvgFillColor(vg, label);
        nvgText(vg, width * .5f, 28, "Browser Transport", nullptr);
        nvgFontSize(vg, 10);
        nvgText(vg, 44, 117, "BPM", nullptr);
        nvgText(vg, 96, 117, "METER", nullptr);
        nvgText(vg, 162, 117, "QUANTIZE", nullptr);
        nvgText(vg, controlX(0), 170, "RUN", nullptr);
        nvgText(vg, controlX(1), 170, "SYNC", nullptr);
        nvgText(vg, controlX(2), 170, "TAP", nullptr);
        nvgText(vg, controlX(1), 246, "IN", nullptr);
        nvgText(vg, 160, 174, "OUT", nullptr);
        const char* outs[] = {"PLAYING", "RESET", "BAR", "BEAT", "STEP", "BAR PH", "BEAT PH", "BPM"};
        for (int i = 0; i < HostTime::kHostTimeCount; ++i) {
            const float x = 138 + 45 * (i % 2);
            const float y = 218 + 39 * (i / 2);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x - 20, y - 25, 40, 36, 4);
            nvgFillColor(vg, nvgRGB(170, 171, 172));
            nvgFill(vg);
            nvgFillColor(vg, nvgRGB(22, 22, 23));
            nvgFontSize(vg, 8);
            nvgText(vg, x, y - 18, outs[i], nullptr);
        }
        ModuleWidget::draw(args);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1) {
            auto* vg = args.vg;
            const Clock clock = module ? browserTransport() : Clock{};
            const auto whole = static_cast<unsigned long long>(std::floor(clock.beat + 1e-9));
            const auto seconds = module && clock.enabled
                ? static_cast<const BrowserTransportModule*>(module)->timeInfo.seconds : 0u;
            char position[32], elapsed[24], tempo[16];
            std::snprintf(position, sizeof(position), "%03llu : %02llu",
                          whole / clock.beatsPerBar + 1, whole % clock.beatsPerBar + 1);
            std::snprintf(elapsed, sizeof(elapsed), "%02u:%02u:%02u",
                          seconds / 3600 % 100, seconds / 60 % 60, seconds % 60);
            std::snprintf(tempo, sizeof(tempo), "%d", clock.bpm);
            if (const auto font = APP->window->loadFont(monoFontPath)) {
                nvgFontFaceId(vg, font->handle);
                nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                nvgFontSize(vg, 28);
                nvgFillColor(vg, nvgRGB(238, 79, 99));
                nvgText(vg, width * .5f, 60, position, nullptr);
                nvgFillColor(vg, nvgRGB(188, 68, 84));
                nvgFontSize(vg, 17);
                nvgText(vg, width * .5f, 87, elapsed, nullptr);
                nvgFontSize(vg, 13);
                nvgText(vg, 44, 160, tempo, nullptr);
            }
        }
        ModuleWidget::drawLayer(args, layer);
    }

    void appendContextMenu(ui::Menu* menu) override {
        menu->addChild(new ui::MenuSeparator);
        menu->addChild(createCheckMenuItem("Browser transport enabled", "",
            [] { return browserTransport().enabled; },
            [] { browserTransport().setEnabled(!browserTransport().enabled); }));
    }
};

Model* modelBrowserTransport = createModel<BrowserTransportModule, BrowserTransportWidget>("BrowserTransport");
