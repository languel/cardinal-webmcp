// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin.hpp"

#if defined(__EMSCRIPTEN__) && !defined(HEADLESS)
#include "ImGuiTextEditor.hpp"

#include <atomic>
#include <emscripten.h>


namespace {

const float compactWidth = 8 * RACK_GRID_WIDTH;
const float expandedWidth = 32 * RACK_GRID_WIDTH;
constexpr size_t maxSourceSize = 16384;
constexpr char defaultSource[] =
    "// Voltages in, voltages out. time is in seconds.\n"
    "function process({ input, time }) {\n"
    "  return [input[0] * 0.5 + input[1], 5 * Math.sin(2 * Math.PI * time)];\n"
    "}\n";

struct CodeModule : Module {
    static constexpr int channels = 2;
    std::atomic<float> inputCV[channels] {{0.f}, {0.f}};
    std::atomic<float> outputCV[channels] {{0.f}, {0.f}};
    std::string source = defaultSource;
    unsigned revision = 0;
    bool expanded = true;

    CodeModule() {
        config(0, channels, channels, 0);
        for (int i = 0; i < channels; ++i) {
            configInput(i, "CV " + std::to_string(i + 1));
            configOutput(i, "CV " + std::to_string(i + 1));
        }
    }

    void process(const ProcessArgs&) override {
        for (int i = 0; i < channels; ++i) {
            inputCV[i].store(inputs[i].getVoltage(), std::memory_order_relaxed);
            outputs[i].setVoltage(outputCV[i].load(std::memory_order_relaxed));
        }
    }

    void onReset(const ResetEvent& e) override {
        Module::onReset(e);
        source = defaultSource;
        ++revision;
    }

    json_t* dataToJson() override {
        return json_pack("{s:s,s:b}", "source", source.c_str(), "expanded", expanded);
    }

    void dataFromJson(json_t* data) override {
        const json_t* value = json_object_get(data, "source");
        const char* loaded = json_is_string(value) && json_string_length(value) <= maxSourceSize
            ? json_string_value(value) : defaultSource;
        if (source != loaded) {
            source = loaded;
            ++revision;
        }
        // The first version only occupied 8 HP.
        expanded = json_is_true(json_object_get(data, "expanded"));
    }
};

enum class RunStatus { Stopped, Starting, Running, Error };

unsigned nextRunner = 0;

struct CodeEditor : ImGuiTextEditor {
    const unsigned runner;
    bool selected = false;
    explicit CodeEditor(unsigned id) : runner(id) {
        setShowStatus(false);
        setLanguageDefinition("JavaScript");
    }

    void onButton(const ButtonEvent& e) override {
        if (e.button != GLFW_MOUSE_BUTTON_LEFT)
            return;
        setAsCurrentContext();
        const float scale = getScaleFactor();
        ImGui::GetIO().MousePos = ImVec2(e.pos.x * scale, e.pos.y * scale);
        ImGuiTextEditor::onButton(e);
        e.consume(this);
    }

    void onSelect(const SelectEvent&) override {
        selected = true;
        EM_ASM({ Module.cardinalCodeEditor = $0; }, runner);
    }

    void onDeselect(const DeselectEvent&) override {
        selected = false;
        EM_ASM({ if (Module.cardinalCodeEditor === $0) Module.cardinalCodeEditor = 0; }, runner);
        setAsCurrentContext();
        ImGui::GetIO().ClearInputKeys();
    }

    void onSelectKey(const SelectKeyEvent& e) override {
        ImGuiTextEditor::onSelectKey(e);
        e.consume(this);
    }
    void step() override {
        if (selected) {
            if (EM_ASM_INT({
                return Module.cardinalCode && Module.cardinalCode.takeSelectAll($0) ? 1 : 0;
            }, runner))
                selectAll();

            char* text = reinterpret_cast<char*>(EM_ASM_PTR({
                if (!Module.cardinalCode || Module.cardinalCodeEditor !== $0) return 0;
                const text = Module.cardinalCode.takePaste($0);
                if (!text) return 0;
                const size = lengthBytesUTF8(text) + 1;
                const buffer = _malloc(size);
                if (buffer) stringToUTF8(text, buffer, size);
                return buffer;
            }, runner));
            if (text) {
                paste(text);
                std::free(text);
            }
        }
        ImGuiTextEditor::step();
    }

};

struct CodeButton : ui::Button {
    std::function<void()> action;
    void onAction(const ActionEvent&) override { action(); }
};

struct CodeWidget : ModuleWidget {
    CodeModule* const code;
    const unsigned runner;
    unsigned revision;
    CodeEditor* editor;
    CodeButton* runButton;
    CodeButton* stopButton;
    CodeButton* layoutButton;
    app::PortWidget* inputJacks[CodeModule::channels];
    app::PortWidget* outputJacks[CodeModule::channels];
    widget::Widget* rightScrews[2];
    RunStatus status = RunStatus::Stopped;
    std::string message;
    std::string submittedSource;

    explicit CodeWidget(CodeModule* module)
        : code(module), runner(module ? ++nextRunner : 0), revision(module ? module->revision : 0) {
        setModule(module);
        box.size.y = RACK_GRID_HEIGHT;
        for (int i = 0; i < 2; ++i) {
            const float y = i ? RACK_GRID_HEIGHT - RACK_GRID_WIDTH : 0.f;
            addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, y)));
            rightScrews[i] = createWidget<ScrewSilver>(Vec(0, y));
            addChild(rightScrews[i]);
        }

        editor = new CodeEditor(runner);
        editor->setText(code ? code->source : defaultSource);
        addChild(editor);
        runButton = addButton("Run", [this] { run(); });
        stopButton = addButton("Stop", [this] { stop(); });
        layoutButton = addButton("Collapse", [this] { toggleLayout(); });

        for (int i = 0; i < CodeModule::channels; ++i) {
            inputJacks[i] = createInputCentered<PJ301MPort>(Vec(0, 195), module, i);
            outputJacks[i] = createOutputCentered<PJ301MPort>(Vec(0, 295), module, i);
            addInput(inputJacks[i]);
            addOutput(outputJacks[i]);
        }
        layout();
    }

    ~CodeWidget() override {
        stop();
        EM_ASM({ if (Module.cardinalCodeEditor === $0) Module.cardinalCodeEditor = 0; }, runner);
    }

    CodeButton* addButton(const char* text, std::function<void()> action) {
        auto* button = new CodeButton;
        button->text = text;
        button->action = std::move(action);
        addChild(button);
        return button;
    }

    float panelWidth() const { return !code || code->expanded ? expandedWidth : compactWidth; }

    void layout() {
        box.size.x = panelWidth();
        const bool expanded = box.size.x == expandedWidth;
        const float rail = box.size.x - compactWidth;
        for (auto* screw : rightScrews)
            screw->box.pos.x = box.size.x - 2 * RACK_GRID_WIDTH;
        for (int i = 0; i < CodeModule::channels; ++i) {
            const float x = rail + 33.f + i * 54.f;
            inputJacks[i]->box.pos.x = x - inputJacks[i]->box.size.x * .5f;
            outputJacks[i]->box.pos.x = x - outputJacks[i]->box.size.x * .5f;
        }
        editor->visible = expanded;
        editor->box = Rect(Vec(12, 75), Vec(expandedWidth - compactWidth - 18, 260));
        runButton->box = Rect(Vec(12, expanded ? 42.f : 105.f), Vec(expanded ? 56.f : 45.f, 24));
        stopButton->box = Rect(Vec(expanded ? 74.f : 63.f, expanded ? 42.f : 105.f), Vec(expanded ? 56.f : 45.f, 24));
        layoutButton->box = Rect(Vec(rail + 12, 42), Vec(96, 24));
        layoutButton->text = expanded ? "Collapse" : "Show editor";
    }

    std::unique_ptr<history::ModuleChange> beginChange(const char* name) {
        auto action = std::unique_ptr<history::ModuleChange>(new history::ModuleChange);
        action->name = name;
        action->moduleId = code->id;
        action->oldModuleJ = toJson();
        action->newModuleJ = nullptr;
        return action;
    }

    void finishChange(std::unique_ptr<history::ModuleChange> action) {
        action->newModuleJ = toJson();
        APP->history->push(action.release());
    }

    void toggleLayout() {
        if (!code)
            return;
        const float oldWidth = box.size.x;
        box.size.x = code->expanded ? compactWidth : expandedWidth;
        if (!APP->scene->rack->requestModulePos(this, box.pos)) {
            box.size.x = oldWidth;
            message = "Make room to the right.";
            return;
        }
        auto action = beginChange("resize code module");
        code->expanded = !code->expanded;
        layout();
        message.clear();
        finishChange(std::move(action));
    }

    void run() {
        if (!code)
            return;
        stop();
        auto source = editor->getText();
        // The editor appends a newline after the final line when reading it.
        if (!source.empty() && source.back() == '\n')
            source.pop_back();
        if (source.size() > maxSourceSize || source.find('\0') != std::string::npos) {
            status = RunStatus::Error;
            message = "Source must be at most 16 KiB and contain no NUL characters.";
            return;
        }
        const bool started = EM_ASM_INT({
            if (!Module.cardinalCode) return 0;
            Module.cardinalCode.run($0, UTF8ToString($1));
            return 1;
        }, runner, source.c_str());
        if (started) {
            submittedSource = std::move(source);
            status = RunStatus::Starting;
        } else {
            status = RunStatus::Error;
            message = "JavaScript runner is unavailable.";
        }
    }

    void stop() {
        if (status == RunStatus::Starting || status == RunStatus::Running) {
            EM_ASM({ if (Module.cardinalCode) Module.cardinalCode.remove($0); }, runner);
            for (auto& voltage : code->outputCV)
                voltage.store(0.f, std::memory_order_relaxed);
        }
        status = RunStatus::Stopped;
        message.clear();
        submittedSource.clear();
    }

    void step() override {
        if (code) {
            if (box.size.x != panelWidth())
                layout();
            if (revision != code->revision) {
                stop();
                editor->setText(code->source);
                revision = code->revision;
            }
            if (status == RunStatus::Starting || status == RunStatus::Running)
                exchangeCV();
        }
        ModuleWidget::step();
    }

    void exchangeCV() {
        float values[4] {
            code->inputCV[0].load(std::memory_order_relaxed),
            code->inputCV[1].load(std::memory_order_relaxed), 0.f, 0.f
        };
        char error[256] = {};
        const auto previous = status;
        status = static_cast<RunStatus>(EM_ASM_INT({
            const index = $1 >> 2;
            const state = Module.cardinalCode.exchange($0, HEAPF32[index], HEAPF32[index + 1]);
            if (!state) return 0;
            HEAPF32[index + 2] = state.output[0];
            HEAPF32[index + 3] = state.output[1];
            if (state.status === 3) stringToUTF8(state.error, $2, $3);
            return state.status;
        }, runner, values, error, sizeof(error)));
        for (int i = 0; i < CodeModule::channels; ++i)
            code->outputCV[i].store(values[i + 2], std::memory_order_relaxed);

        if (status == RunStatus::Error) {
            EM_ASM({ Module.cardinalCode.remove($0); }, runner);
            message = error;
            submittedSource.clear();
        } else if (status == RunStatus::Running && previous == RunStatus::Starting) {
            if (submittedSource != code->source) {
                auto action = beginChange("edit module code");
                code->source = submittedSource;
                finishChange(std::move(action));
            }
            submittedSource.clear();
        }
    }

    void draw(const DrawArgs& args) override {
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(args.vg, nvgRGB(28, 32, 36));
        nvgFill(args.vg);

        const float rail = box.size.x - compactWidth;
        nvgFillColor(args.vg, nvgRGB(222, 227, 231));
        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontSize(args.vg, 15);
        nvgText(args.vg, box.size.x * .5f, 26, "Code", nullptr);
        nvgFontSize(args.vg, 11);
        nvgText(args.vg, rail + 60, 153, "CV IN", nullptr);
        nvgText(args.vg, rail + 60, 253, "CV OUT", nullptr);
        for (float y : {173.f, 273.f}) {
            nvgText(args.vg, rail + 33, y, "1", nullptr);
            nvgText(args.vg, rail + 87, y, "2", nullptr);
        }

        const char* labels[] = {"Stopped", "Starting…", "Running", "Error"};
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        if (editor->visible) {
            nvgText(args.vg, 143, 50, labels[static_cast<int>(status)], nullptr);
            if (!message.empty()) {
                nvgFillColor(args.vg, nvgRGB(248, 171, 146));
                nvgTextBox(args.vg, 12, 340, rail - 14, message.c_str(), nullptr);
            }
        } else {
            nvgTextBox(args.vg, 12, 77, 96, message.empty()
                ? labels[static_cast<int>(status)] : message.c_str(), nullptr);
        }
        ModuleWidget::draw(args);
    }
};

} // namespace

Model* modelCardinalCode = createModel<CodeModule, CodeWidget>("Code");
#endif
