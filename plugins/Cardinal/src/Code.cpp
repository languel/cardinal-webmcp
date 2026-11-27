// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin.hpp"

#if defined(__EMSCRIPTEN__) && !defined(HEADLESS)
#include "ImGuiTextEditor.hpp"
#include "LivecodePanel.hpp"
#include <RackNavigation.hpp>
#include <BrowserTransport.hpp>
#include "ShaderSource.hpp"
#include <AbsoluteSlider.hpp>

#include <GLES3/gl3.h>
#include <dsp/ringbuffer.hpp>
#include <ctime>

#include <atomic>
#include <emscripten.h>


namespace rack {
namespace app {
bool handleRoomBrightnessKey(int key, int mods, int action);
}
}

namespace {

const float compactWidth = 8 * RACK_GRID_WIDTH;
const float expandedWidth = 32 * RACK_GRID_WIDTH;
constexpr size_t maxSourceSize = 16384;
constexpr char defaultSource[] =
    "// Voltages in, voltages out. time is in seconds.\n"
    "function process({ input, time }) {\n"
    "  return [input[0] * 0.5 + input[1], 5 * Math.sin(2 * Math.PI * time)];\n"
    "}\n";

constexpr int maxCV = 6;
constexpr int defaultCV = 2;

int loadJackCount(json_t* data, const char* key) {
    auto* value = json_object_get(data, key);
    if (!json_is_integer(value))
        return defaultCV;
    const auto count = json_integer_value(value);
    return count < 0 || count > maxCV ? defaultCV : static_cast<int>(count);
}

void addJackCountMenu(ui::Menu* menu, const char* label, ModuleWidget* widget,
                      bool input, int& count) {
    auto* module = widget->getModule();
    int minimum = 0;
    for (int i = 0; i < maxCV; ++i) {
        if ((input ? module->inputs[i].isConnected() : module->outputs[i].isConnected()))
            minimum = i + 1;
    }
    auto* countPtr = &count;
    menu->addChild(createSubmenuItem(label, std::to_string(count), [=](ui::Menu* submenu) {
        for (int n = 0; n <= maxCV; ++n) {
            submenu->addChild(createCheckMenuItem(std::to_string(n), "",
                [=] { return *countPtr == n; }, [=] {
                    if (*countPtr != n) {
                        auto* action = new history::ModuleChange;
                        action->name = "change CV jack count";
                        action->moduleId = module->id;
                        action->oldModuleJ = widget->toJson();
                        *countPtr = n;
                        action->newModuleJ = widget->toJson();
                        APP->history->push(action);
                    }
                }, n < minimum));
        }
    }));
}

template <typename KeyEvent>
bool handleRunShortcut(const KeyEvent& e, const std::function<void()>& run,
                       const std::function<void()>& stop) {
    const int modifiers = e.mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER | GLFW_MOD_SHIFT | GLFW_MOD_ALT);
    if (modifiers != GLFW_MOD_CONTROL && modifiers != GLFW_MOD_SUPER)
        return false;
    if (e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER) {
        if (e.action == GLFW_PRESS && run)
            run();
        return true;
    }
    if (e.key == GLFW_KEY_PERIOD || e.keyName == ".") {
        if (e.action == GLFW_PRESS && stop)
            stop();
        return true;
    }
    return false;
}

struct CodeModule : Module {
    static constexpr int channels = maxCV;
    int inputCount = defaultCV;
    int outputCount = defaultCV;
    std::atomic<float> inputCV[channels] {};
    std::atomic<float> outputCV[channels] {};
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
        return json_pack("{s:s,s:b,s:i,s:i}", "source", source.c_str(), "expanded", expanded,
                         "inputs", inputCount, "outputs", outputCount);
    }

    void dataFromJson(json_t* data) override {
        const json_t* value = json_object_get(data, "source");
        const char* loaded = json_is_string(value) && json_string_length(value) <= maxSourceSize
            ? json_string_value(value) : defaultSource;
        if (source != loaded) {
            source = loaded;
            ++revision;
        }
        inputCount = loadJackCount(data, "inputs");
        outputCount = loadJackCount(data, "outputs");
        // The first version only occupied 8 HP.
        expanded = json_is_true(json_object_get(data, "expanded"));
    }
};

enum class RunStatus { Stopped, Starting, Running, Error };

unsigned nextRunner = 0;

struct EditorSlider : ui::AbsoluteSlider {
    struct Setting : Quantity {
        std::string label;
        float& value;
        std::function<void()> changed;

        Setting(std::string label, float& value, std::function<void()> changed)
            : label(std::move(label)), value(value), changed(std::move(changed)) {}

        float getValue() override { return value; }
        void setValue(float next) override {
            value = math::clamp(next, 0.f, 1.f);
            changed();
        }
        float getMinValue() override { return 0.f; }
        float getMaxValue() override { return 1.f; }
        float getDisplayValue() override { return value * 100.f; }
        void setDisplayValue(float next) override { setValue(next / 100.f); }
        std::string getLabel() override { return label; }
        std::string getUnit() override { return "%"; }
    };

    Setting setting;

    EditorSlider(std::string label, float& value, std::function<void()> changed)
        : setting(std::move(label), value, std::move(changed)) {
        quantity = &setting;
        box.size.x = 190.f;
    }
};

struct CodeEditor : ImGuiTextEditor {
    const unsigned runner;
    const bool glsl;
    std::string formatSource;
    std::string formatError;
    bool formatPending = false;
    bool selected = false;
    bool lineNumbers = true;
    bool wrap = false;
    bool whitespace = false;
    int fontPercent = 100;
    int theme = 0;
    int font = 0;
    float codeOpacity = 1.f;
    bool glyphBackground = false;
    ImVec4 glyphColor = ImVec4(0.f, 0.f, 0.f, .65f);
    static constexpr const char* themes[] = {"Desaturated", "Monochrome", "Syntax colors"};
    std::function<void()> runAction, stopAction;
    explicit CodeEditor(unsigned id, const char* language = "JavaScript")
        : runner(id), glsl(std::strcmp(language, "GLSL") == 0) {
        transparentBackground = true;
        setShowStatus(false);
        textEditor().SetAutoHorizontalScrollbar(true);
        applyAppearance();
        setLanguageDefinition(language);
    }

    ~CodeEditor() override {
        EM_ASM({ if (Module.cardinalCode) Module.cardinalCode.discardEditorCommands($0); }, runner);
    }

    void setPlaybackMarks(const TextEditor::PlaybackMark* marks = nullptr, size_t count = 0) {
        textEditor().SetPlaybackMarks(marks, count);
    }

    void toggleWrap() {
        wrap = !wrap;
        applyAppearance();
    }

    void format() {
        formatSource = getText();
        formatError.clear();
        formatPending = EM_ASM_INT({
            if (!Module.cardinalCode) return 0;
            Module.cardinalCode.formatEditor($0, !!$1, UTF8ToString($2));
            return 1;
        }, runner, glsl, formatSource.c_str());
    }

    void finishFormat() {
        int failed = 0;
        char* value = reinterpret_cast<char*>(EM_ASM_PTR({
            const result = Module.cardinalCode.takeFormat($0);
            if (!result) return 0;
            const text = result.error || result.text;
            HEAP32[$1 >> 2] = result.error ? 1 : 0;
            const size = lengthBytesUTF8(text) + 1;
            const buffer = _malloc(size);
            if (buffer) stringToUTF8(text, buffer, size);
            return buffer;
        }, runner, &failed));
        if (!value)
            return;
        formatPending = false;
        const std::string formatted(value);
        std::free(value);
        if (getText() != formatSource)
            return;
        if (failed) {
            formatError = formatted;
        } else if (formatted.size() > maxSourceSize) {
            formatError = "Formatted code is larger than 16 KiB.";
        } else if (!formatted.empty() && formatted != formatSource) {
            selectAll();
            paste(formatted.c_str());
        }
    }

    void applyAppearance() {
        auto palette = TextEditor::GetDarkPalette();
        using Color = TextEditor::PaletteIndex;
        for (int i = 0; i < static_cast<int>(Color::Max); ++i) {
            auto color = ImGui::ColorConvertU32ToFloat4(palette[i]);
            const float gray = color.x * .2126f + color.y * .7152f + color.z * .0722f;
            const float saturation = theme == 0 ? .38f : theme == 1 ? 0.f : 1.f;
            color.x = gray + (color.x - gray) * saturation;
            color.y = gray + (color.y - gray) * saturation;
            color.z = gray + (color.z - gray) * saturation;
            if (i <= static_cast<int>(Color::MultiLineComment) || i == static_cast<int>(Color::LineNumber))
                color.w *= codeOpacity;
            palette[i] = ImGui::ColorConvertFloat4ToU32(color);
        }
        palette[static_cast<int>(Color::Background)] = 0;
        palette[static_cast<int>(Color::CurrentLineFill)] = IM_COL32(255, 255, 255, 12);
        palette[static_cast<int>(Color::CurrentLineFillInactive)] = 0;
        palette[static_cast<int>(Color::CurrentLineEdge)] = 0;
        textEditor().SetPalette(palette);
        textEditor().SetShowLineNumbers(lineNumbers);
        textEditor().SetShowWhitespaces(whitespace);
        textEditor().SetWrap(wrap);
        textEditor().SetFontScale(fontPercent / 100.f);
        textEditor().SetGlyphBackground(glyphBackground ? ImGui::ColorConvertFloat4ToU32(glyphColor) : 0);
        setCodeFont(font);
    }

    void changeFont(int delta) {
        fontPercent = math::clamp(fontPercent + delta, 70, 200);
        applyAppearance();
    }

    void appendAppearanceMenu(ui::Menu* menu) {
        menu->addChild(createSubmenuItem("Theme", themes[theme], [this](ui::Menu* submenu) {
            for (int index = 0; index < 3; ++index) {
                submenu->addChild(createCheckMenuItem(themes[index], "",
                    [this, index] { return theme == index; },
                    [this, index] { theme = index; applyAppearance(); }));
            }
        }));
        menu->addChild(createCheckMenuItem("Line numbers", "",
            [this] { return lineNumbers; }, [this] { lineNumbers = !lineNumbers; applyAppearance(); }));
        menu->addChild(createCheckMenuItem("Wrap text", "Alt+Z",
            [this] { return wrap; }, [this] { toggleWrap(); }));
        menu->addChild(createMenuItem("Format code", "Alt+Shift+F", [this] { format(); }));
        menu->addChild(createCheckMenuItem("Show whitespace", "",
            [this] { return whitespace; }, [this] { whitespace = !whitespace; applyAppearance(); }));
        static constexpr const char* fonts[] = {"Share Tech Mono", "Roboto Mono", "Fira Mono Bold", "Proggy Clean"};
        menu->addChild(createSubmenuItem("Font", fonts[font], [this](ui::Menu* submenu) {
            for (int index = 0; index < 4; ++index) {
                submenu->addChild(createCheckMenuItem(fonts[index], "",
                    [this, index] { return font == index; },
                    [this, index] { font = index; applyAppearance(); }));
            }
        }));
        menu->addChild(new EditorSlider("Code opacity", codeOpacity, [this] { applyAppearance(); }));
        menu->addChild(createSubmenuItem("Text background", glyphBackground ? "On" : "Off",
            [this](ui::Menu* submenu) {
                submenu->addChild(createCheckMenuItem("Enabled", "",
                    [this] { return glyphBackground; },
                    [this] { glyphBackground = !glyphBackground; applyAppearance(); }));
                auto changed = [this] { applyAppearance(); };
                submenu->addChild(new EditorSlider("Opacity", glyphColor.w, changed));
                submenu->addChild(new EditorSlider("Red", glyphColor.x, changed));
                submenu->addChild(new EditorSlider("Green", glyphColor.y, changed));
                submenu->addChild(new EditorSlider("Blue", glyphColor.z, changed));
            }));
        menu->addChild(createSubmenuItem("Font size", std::to_string(fontPercent) + "%", [this](ui::Menu* submenu) {
            submenu->addChild(createMenuItem("Increase", RACK_MOD_CTRL_NAME "+Shift++", [this] { changeFont(10); }));
            submenu->addChild(createMenuItem("Decrease", RACK_MOD_CTRL_NAME "+Shift+-", [this] { changeFont(-10); }));
            submenu->addChild(createMenuItem("Reset", "", [this] { fontPercent = 100; applyAppearance(); }));
        }));
    }

    void draw(const DrawArgs& args) override {
        if (args.fb)
            ImGuiTextEditor::draw(args);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1 && !args.fb)
            ImGuiTextEditor::draw(args);
        ImGuiTextEditor::drawLayer(args, layer);
    }

    void onButton(const ButtonEvent& e) override {
        if (e.button == GLFW_MOUSE_BUTTON_RIGHT && e.action == GLFW_PRESS) {
            appendAppearanceMenu(createMenu());
            e.consume(this);
            return;
        }
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
        const int mods = e.mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER | GLFW_MOD_SHIFT | GLFW_MOD_ALT);
        const bool fontShortcut = mods == (GLFW_MOD_SUPER | GLFW_MOD_SHIFT)
            || mods == (GLFW_MOD_CONTROL | GLFW_MOD_SHIFT);
        const bool smaller = e.key == GLFW_KEY_MINUS || e.key == '_' || e.key == GLFW_KEY_KP_SUBTRACT;
        const bool larger = e.key == GLFW_KEY_EQUAL || e.key == '+' || e.key == GLFW_KEY_KP_ADD;
        if (fontShortcut && (smaller || larger)) {
            if (e.action != GLFW_RELEASE)
                changeFont(smaller ? -10 : 10);
        }
        else if (app::handleRoomBrightnessKey(e.key, e.mods, e.action)) {
            e.consume(this);
            return;
        }
        else if (!handleRunShortcut(e, runAction, stopAction))
            ImGuiTextEditor::onSelectKey(e);
        e.consume(this);
    }
    void step() override {
        for (int command = 0; (command = EM_ASM_INT({
            return Module.cardinalCode ? Module.cardinalCode.takeEditorCommand($0) : 0;
        }, runner));) {
            if (command == 1) toggleWrap();
            else if (command == 2) undo();
            else if (command == 3) redo();
            else if (command == 4) format();
        }
        if (formatPending)
            finishFormat();
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

// Keep the panel face quiet and reserve the center for code and output.
constexpr float jackColumnWidth = 48.f;
constexpr float jackHeadingY = 86.f;
constexpr float firstJackY = 110.f;
constexpr float jackSpacing = 42.f;

void drawCodePanel(NVGcontext* vg, Vec size) {
    drawLivecodePanel(vg, size);
    nvgFontFaceId(vg, APP->window->uiFont->handle);
    nvgFontSize(vg, 11);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, settings::preferDarkPanels ? nvgRGB(202, 202, 202) : nvgRGB(62, 62, 62));
}

void drawJackColumn(NVGcontext* vg, float x, const char* label, int count, bool output,
                    float headingY = jackHeadingY) {
    if (count == 0)
        return;
    nvgText(vg, x, headingY, label, nullptr);
    if (!output)
        return;

    nvgSave(vg);
    const float firstY = headingY + firstJackY - jackHeadingY;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x - 16.f, firstY - 17.f, 32.f, (count - 1) * jackSpacing + 36.f, 5.f);
    nvgFillPaint(vg, nvgLinearGradient(vg, x - 16.f, firstY, x + 16.f, firstY,
                                      nvgRGB(166, 166, 168), nvgRGB(112, 112, 115)));
    nvgFill(vg);
    nvgRestore(vg);
}

struct CodeButton : ui::Button {
    std::function<void()> action;
    bool lit = false;
    app::LightWidget* const light = new app::LightWidget;

    CodeButton() {
        light->box = Rect(Vec(7.5f, 7.5f), Vec(7.f, 7.f));
        addChild(light);
    }

    void onAction(const ActionEvent&) override { action(); }

    void draw(const DrawArgs& args) override {
        const float x = 11.f;
        const float y = box.size.y * .5f;
        nvgBeginPath(args.vg);
        nvgCircle(args.vg, x, y + 1.f, 9.f);
        nvgFillColor(args.vg, nvgRGB(8, 8, 8));
        nvgFill(args.vg);
        nvgBeginPath(args.vg);
        nvgCircle(args.vg, x, y, 7.5f);
        nvgFillColor(args.vg, nvgRGB(63, 63, 65));
        nvgFill(args.vg);
        nvgStrokeWidth(args.vg, 1.f);
        nvgStrokeColor(args.vg, nvgRGB(115, 115, 118));
        nvgStroke(args.vg);
        nvgBeginPath(args.vg);
        nvgCircle(args.vg, x, y, 3.5f);
        nvgFillColor(args.vg, nvgRGB(103, 103, 105));
        nvgFill(args.vg);
        light->color = lit ? nvgRGB(243, 82, 74) : nvgRGBA(0, 0, 0, 0);
        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgFontSize(args.vg, 10.f);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, nvgRGB(222, 227, 231));
        nvgText(args.vg, 23.f, y, text.c_str(), nullptr);
    }
};

struct CodeWidget : ModuleWidget {
    CodeModule* const code;
    const unsigned runner;
    unsigned revision;
    CodeEditor* editor;
    CodeButton* runButton;
    CodeButton* layoutButton = nullptr;
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
            addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, y)));
            rightScrews[i] = createWidget<ThemedScrew>(Vec(0, y));
            addChild(rightScrews[i]);
        }

        editor = new CodeEditor(runner);
        editor->setText(code ? code->source : defaultSource);
        addChild(editor);
        editor->runAction = [this] { run(); };
        editor->stopAction = [this] { stop(); };
        runButton = addButton("▶", [this] {
            if (status == RunStatus::Running || status == RunStatus::Starting)
                stop();
            else
                run();
        });
        layoutButton = addButton("Collapse", [this] { toggleLayout(); });

        for (int i = 0; i < CodeModule::channels; ++i) {
            inputJacks[i] = createInputCentered<PJ301MPort>(Vec(0, firstJackY + i * jackSpacing), module, i);
            outputJacks[i] = createOutputCentered<PJ301MPort>(Vec(0, firstJackY + i * jackSpacing), module, i);
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
        for (auto* screw : rightScrews)
            screw->box.pos.x = box.size.x - 2 * RACK_GRID_WIDTH;
        for (int i = 0; i < CodeModule::channels; ++i) {
            const float jackY = firstJackY + i * jackSpacing + (expanded ? 0.f : 30.f);
            inputJacks[i]->box.pos = Vec(jackColumnWidth * .5f, jackY) - inputJacks[i]->box.size * .5f;
            outputJacks[i]->box.pos = Vec(box.size.x - jackColumnWidth * .5f, jackY) - outputJacks[i]->box.size * .5f;
        }
        editor->visible = expanded;
        editor->box = Rect(Vec(jackColumnWidth, 35), Vec(expandedWidth - 2 * jackColumnWidth, 320));
        runButton->box = Rect(Vec(expanded ? 48.f : 8.f, expanded ? 8.f : 47.f), Vec(52, 22));
        layoutButton->box = Rect(Vec(expanded ? expandedWidth - 135.f : 8.f, expanded ? 8.f : 23.f),
                                 Vec(98, 22));
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

    void onHoverKey(const HoverKeyEvent& e) override {
        if (handleRunShortcut(e, editor->runAction, editor->stopAction))
            e.consume(this);
        else
            ModuleWidget::onHoverKey(e);
    }

    void appendContextMenu(ui::Menu* menu) override {
        menu->addChild(createSubmenuItem("Editor", "", [this](ui::Menu* submenu) {
            editor->appendAppearanceMenu(submenu);
        }));
        if (!code)
            return;
        addJackCountMenu(menu, "Inputs", this, true, code->inputCount);
        addJackCountMenu(menu, "Outputs", this, false, code->outputCount);
    }

    void step() override {
        for (int i = 0; i < maxCV; ++i) {
            inputJacks[i]->visible = i < (code ? code->inputCount : defaultCV)
                || (code && code->inputs[i].isConnected());
            outputJacks[i]->visible = i < (code ? code->outputCount : defaultCV)
                || (code && code->outputs[i].isConnected());
        }
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
        float values[2 * maxCV] {};
        for (int i = 0; i < code->inputCount; ++i)
            values[i] = code->inputCV[i].load(std::memory_order_relaxed);
        char error[256] = {};
        const auto previous = status;
        status = static_cast<RunStatus>(EM_ASM_INT({
            const index = $1 >> 2;
            const input = Array.from(HEAPF32.subarray(index, index + $4));
            const state = Module.cardinalCode.exchange($0, input, $5);
            if (!state) return 0;
            for (let i = 0; i < $5; ++i)
                HEAPF32[index + $6 + i] = state.output[i] || 0;
            if (state.status === 3) stringToUTF8(state.error, $2, $3);
            return state.status;
        }, runner, values, error, sizeof(error), code->inputCount, code->outputCount, maxCV));
        for (int i = 0; i < CodeModule::channels; ++i)
            code->outputCV[i].store(values[i + maxCV], std::memory_order_relaxed);

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
        drawCodePanel(args.vg, box.size);
        const float headingY = jackHeadingY + (editor->visible ? 0.f : 30.f);
        drawJackColumn(args.vg, jackColumnWidth * .5f, "IN", code ? code->inputCount : defaultCV, false, headingY);
        drawJackColumn(args.vg, box.size.x - jackColumnWidth * .5f, "OUT", code ? code->outputCount : defaultCV, true, headingY);
        runButton->lit = status == RunStatus::Running;

        const auto& notice = editor->formatError.empty() ? message : editor->formatError;
        if (!notice.empty()) {
            const float padding = editor->visible ? jackColumnWidth : 12.f;
            nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgFillColor(args.vg, nvgRGB(115, 105, 103));
            nvgSave(args.vg);
            nvgIntersectScissor(args.vg, padding, 356, box.size.x - 2 * padding, 22);
            nvgTextBox(args.vg, padding, 356, box.size.x - 2 * padding, notice.c_str(), nullptr);
            nvgRestore(args.vg);
        }
        ModuleWidget::draw(args);
    }
};

constexpr char defaultShader[] =
    "precision mediump float;\n"
    "uniform vec2 u_resolution;\n"
    "uniform vec2 u_cv;\n"
    "uniform float u_time;\n"
    "\n"
    "void main() {\n"
    "  vec2 p = (2.0 * gl_FragCoord.xy\n"
    "            - u_resolution) / u_resolution.y;\n"
    "  float radius = 0.4 + 0.055 * u_cv.x;\n"
    "  float phase = u_time + u_cv.y * 0.2;\n"
    "  float wave = 0.075 * cos(8.0 * atan(p.y, p.x) + phase * 2.0);\n"
    "  float ring = 1.0 - smoothstep(0.005, 0.055,\n"
    "    abs(length(p) - radius - wave));\n"
    "  vec3 color = 0.55 + 0.4 * cos(phase + vec3(0.0, 2.1, 4.2));\n"
    "  gl_FragColor = vec4(vec3(0.018, 0.024, 0.04) + ring * color, 1.0);\n"
    "}\n";

enum class VisualRuntime { GLSL, P5, Three, ASCII, Strudel };

bool hasStereoAudio(VisualRuntime runtime) {
    return runtime == VisualRuntime::GLSL || runtime == VisualRuntime::Strudel;
}

const char* visualSource(VisualRuntime kind) {
    switch (kind) {
    case VisualRuntime::Strudel:
        return R"JS(setcpm(120 / 4)
$: note("c3 [eb3 g3] bb2 [g3 eb3]").s("triangle").gain(0.3)
)JS";
    case VisualRuntime::P5:
        return R"JS(function setup() {
  createCanvas(rack.width, rack.height);
}

function draw() {
  background(16, 18, 26);
  noFill();
  stroke(125, 205, 255);
  strokeWeight(3);
  const wave = sin(rack.time * 2);
  const radius = 80 + 23 * wave + 5 * (rack.input[0] || 0);
  circle(width / 2, height / 2, radius * 2);
  rack.output[0] = wave * 5;
})JS";
    case VisualRuntime::Three:
        return R"JS(const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(45, rack.width / rack.height, 0.1, 100);
const mesh = new THREE.Mesh(
  new THREE.TorusKnotGeometry(1, 0.28, 96, 12),
  new THREE.MeshNormalMaterial()
);
scene.add(mesh);
camera.position.z = 5;

function draw() {
  camera.aspect = rack.width / rack.height;
  camera.updateProjectionMatrix();
  mesh.rotation.x = rack.time * 0.7;
  mesh.rotation.y = rack.time * 0.9 + (rack.input[0] || 0) * 0.2;
  renderer.render(scene, camera);
  rack.output[0] = Math.sin(mesh.rotation.y) * 5;
})JS";
    case VisualRuntime::ASCII:
        return R"JS(export const settings = { cols: 64, rows: 24, backgroundColor: "#13151b" };

export function main({ x, y }, context) {
  const u = (x / context.cols - 0.5) * 2;
  const v = y / context.rows - 0.5;
  const wave = Math.sin(10 * Math.hypot(u, v) - context.time / 260);
  const ramp = " .:-=+*#%@";
  return { char: ramp[Math.floor((wave + 1) * 4.49)], color: "#aee9db" };
}

export function post(context) {
  rack.output[0] = Math.sin(context.time / 260) * 5;
})JS";
    default:
        return defaultShader;
    }
}

enum class ShaderLayout { SideBySide, PreviewOnly, Overlay };
constexpr const char* shaderLayouts[] = {"Side by side", "Preview only", "Overlay"};

constexpr int strudelChannels = 4;
constexpr int strudelTransferFrames = 512;

struct StrudelAudio {
    enum Input { Clock, Left, Right, InputCount };
    struct Frame { float samples[strudelChannels] {}; };
    std::atomic<unsigned> runner{0};
    std::atomic<int> pulsesPerBeat{1};
    dsp::SchmittTrigger clock;
    int64_t lastPulse = -1;
    float cps = 0.f;
    std::array<float, strudelTransferFrames * 2> input {};
    std::array<Frame, 4096> capture;
    dsp::RingBuffer<Frame, 8192> queue;
    Frame previous, tail;
    unsigned currentRunner = 0;
    int inputFrames = 0;
    int transferCountdown = 0;
    static constexpr int ramp = 64;
    int rampIn = 0, rampOut = 0;
    bool primed = false;

    Frame process(Module& module, const Module::ProcessArgs& args) {
        const unsigned id = runner.load(std::memory_order_relaxed);
        if (currentRunner != id) {
            currentRunner = id;
            queue.clear();
            inputFrames = transferCountdown = 0;
            primed = false;
            tail = previous;
            rampOut = ramp;
        }

        if (!module.inputs[Clock].isConnected()) {
            clock.reset();
            lastPulse = -1;
            cps = 0.f;
        } else if (clock.process(rescale(module.inputs[Clock].getVoltage(), .1f, 2.f, 0.f, 1.f))) {
            if (lastPulse >= 0) {
                const auto interval = args.frame - lastPulse;
                if (interval >= args.sampleRate / 500.f && interval <= args.sampleRate * 12.f)
                    cps = args.sampleRate / (interval * 4.f * pulsesPerBeat.load(std::memory_order_relaxed));
            }
            lastPulse = args.frame;
        }

        if (id && --transferCountdown <= 0) {
            const unsigned room = capture.size() - queue.size();
            const int count = EM_ASM_INT({
                const start = $1 >> 2;
                const output = HEAPF32.subarray(start, start + $2 * 4);
                const input = HEAPF32.subarray($3 >> 2, ($3 >> 2) + $4 * 2);
                return Module.cardinalCode.transferStrudel($0, output, input, $5, $6);
            }, id, capture.data(), room, input.data(), inputFrames, args.sampleRate, cps);
            if (count > 0)
                queue.pushBuffer(capture.data(), count);
            inputFrames = 0;
            transferCountdown = strudelTransferFrames;
        }
        if (id && (module.inputs[Left].isConnected() || module.inputs[Right].isConnected())) {
            const float left = module.inputs[Left].getVoltage();
            const float right = module.inputs[Right].isConnected() ? module.inputs[Right].getVoltage() : left;
            input[inputFrames * 2] = math::clamp(left * .2f, -1.f, 1.f);
            input[inputFrames * 2 + 1] = math::clamp(right * .2f, -1.f, 1.f);
            ++inputFrames;
        }
        if (id && !primed && queue.size() >= 2048) {
            primed = true;
            rampIn = 0;
        }
        if (primed && queue.empty()) {
            primed = false;
            tail = previous;
            rampOut = ramp;
        }
        Frame sample;
        if (primed) {
            sample = queue.shift();
            if (rampIn < ramp) {
                const float gain = float(++rampIn) / ramp;
                for (auto& channel : sample.samples)
                    channel *= gain;
            }
        } else if (rampOut > 0) {
            for (int channel = 0; channel < strudelChannels; ++channel)
                sample.samples[channel] = tail.samples[channel] * (float(rampOut) / ramp);
            --rampOut;
        }
        previous = sample;
        return sample;
    }
};

struct ShaderModule : Module {
    const VisualRuntime runtime;
    std::atomic<float> cv[maxCV] {};
    std::atomic<float> visualOutput[maxCV] {};
    int outputCount = defaultCV;
    int inputCount = defaultCV;
    std::string source;
    unsigned revision = 0;
    ShaderLayout layout = ShaderLayout::SideBySide;
    struct Stereo { float left, right; };
    dsp::RingBuffer<Stereo, 8192> sound;
    std::atomic<float> sampleRate{0.f};
    std::atomic<unsigned> soundGeneration{0};
    std::atomic<unsigned> soundReady{0};
    std::atomic<bool> soundRunning{false};
    std::unique_ptr<StrudelAudio> strudel;
    bool followBrowserTransport = true;

    explicit ShaderModule(VisualRuntime kind = VisualRuntime::GLSL) : runtime(kind), source(visualSource(kind)) {
        if (runtime == VisualRuntime::Strudel) {
            layout = ShaderLayout::Overlay;
            strudel.reset(new StrudelAudio);
            config(0, StrudelAudio::InputCount, strudelChannels, 0);
            configInput(StrudelAudio::Clock, "External tempo clock");
            configInput(StrudelAudio::Left, "Audio left (normalled to right)");
            configInput(StrudelAudio::Right, "Audio right");
            for (int i = 0; i < strudelChannels; ++i)
                configOutput(i, "Audio channel " + std::to_string(i + 1));
            return;
        }
        if (hasStereoAudio(runtime)) {
            config(0, maxCV, 2, 0);
            configOutput(0, "Audio left");
            configOutput(1, "Audio right");
        } else {
            config(0, maxCV, maxCV, 0);
            for (int i = 0; i < maxCV; ++i)
                configOutput(i, "CV " + std::to_string(i + 1));
        }
        for (int i = 0; i < int(inputs.size()); ++i)
            configInput(i, "Shader CV " + std::to_string(i + 1));
    }

    void process(const ProcessArgs& args) override {
        if (strudel) {
            sampleRate.store(args.sampleRate, std::memory_order_relaxed);
            const auto sample = strudel->process(*this, args);
            for (int i = 0; i < strudelChannels; ++i)
                outputs[i].setVoltage(sample.samples[i] * 5.f);
            return;
        }
        for (size_t i = 0; i < inputs.size(); ++i)
            cv[i].store(inputs[i].getVoltage(), std::memory_order_relaxed);
        if (!hasStereoAudio(runtime)) {
            for (int i = 0; i < maxCV; ++i)
                outputs[i].setVoltage(visualOutput[i].load(std::memory_order_relaxed));
            return;
        }
        sampleRate.store(args.sampleRate, std::memory_order_relaxed);
        const unsigned generation = soundGeneration.load();
        if (generation != soundReady.load(std::memory_order_relaxed)) {
            sound.clear();
            soundReady.store(generation);
        }
        const Stereo sample = soundRunning.load(std::memory_order_relaxed) && !sound.empty()
            ? sound.shift() : Stereo{};
        outputs[0].setVoltage(sample.left * 5.f);
        outputs[1].setVoltage(sample.right * 5.f);
    }

    json_t* dataToJson() override {
        auto* data = json_pack("{s:s,s:i,s:i,s:i}", "source", source.c_str(),
                               "layout", static_cast<int>(layout), "inputs", inputCount, "outputs", outputCount);
        if (strudel) {
            json_object_set_new(data, "strudelVisualLayout", json_true());
            json_object_set_new(data, "pulsesPerBeat", json_integer(strudel->pulsesPerBeat.load()));
            json_object_set_new(data, "followBrowserTransport", json_boolean(followBrowserTransport));
        }
        return data;
    }

    void dataFromJson(json_t* data) override {
        const json_t* text = json_object_get(data, "source");
        const char* loaded = json_is_string(text) && json_string_length(text) <= maxSourceSize
            ? json_string_value(text) : visualSource(runtime);
        if (source != loaded) {
            source = loaded;
            ++revision;
        }
        inputCount = loadJackCount(data, "inputs");
        outputCount = loadJackCount(data, "outputs");
        if (strudel) {
            const auto saved = json_integer_value(json_object_get(data, "pulsesPerBeat"));
            strudel->pulsesPerBeat.store(saved >= 1 && saved <= 48 ? int(saved) : 1);
            followBrowserTransport = !json_is_false(json_object_get(data, "followBrowserTransport"));
        }
        const auto* savedLayout = json_object_get(data, "layout");
        const auto value = json_integer_value(savedLayout);
        if (runtime == VisualRuntime::Strudel && !json_is_true(json_object_get(data, "strudelVisualLayout"))) {
            layout = ShaderLayout::Overlay;
        } else {
            layout = value >= 0 && value <= 2 ? static_cast<ShaderLayout>(value) : ShaderLayout::SideBySide;
        }
        if (!savedLayout && json_is_true(json_object_get(data, "previewOnly")))
            layout = ShaderLayout::PreviewOnly;
    }

    void onReset(const ResetEvent& e) override {
        Module::onReset(e);
        source = visualSource(runtime);
        ++revision;
    }
};

std::string shaderLog(GLuint object, bool linking) {
    GLint length = 0;
    if (linking)
        glGetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
    else
        glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
    if (length < 2)
        return linking ? "Shader link failed." : "Shader compilation failed.";
    std::vector<GLchar> buffer(length);
    if (linking)
        glGetProgramInfoLog(object, length, nullptr, buffer.data());
    else
        glGetShaderInfoLog(object, length, nullptr, buffer.data());
    return buffer.data();
}

GLuint compileShader(GLenum type, const char* source, std::string& error) {
    const GLuint shader = glCreateShader(type);
    if (!shader) {
        error = "Could not create an OpenGL shader.";
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success)
        return shader;
    error = shaderLog(shader, false);
    glDeleteShader(shader);
    return 0;
}

GLuint linkShader(const std::string& source, std::string& error) {
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, source.c_str(), error);
    if (!fragment)
        return 0;
    const bool modern = source.find("#version 300 es") != std::string::npos;
    const GLuint vertex = compileShader(GL_VERTEX_SHADER, modern
        ? "#version 300 es\nin vec2 a_position;\nout vec2 v_uv;\n"
          "void main() { v_uv = a_position * .5 + .5; gl_Position = vec4(a_position, 0., 1.); }"
        : "attribute vec2 a_position;\nvoid main() { gl_Position = vec4(a_position, 0., 1.); }",
        error);
    if (!vertex) {
        glDeleteShader(fragment);
        return 0;
    }
    const GLuint program = glCreateProgram();
    if (program) {
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glBindAttribLocation(program, 0, "a_position");
        glLinkProgram(program);
    }
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint success = GL_FALSE;
    if (program)
        glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (success)
        return program;
    error = program ? shaderLog(program, true) : "Could not create an OpenGL program.";
    if (program)
        glDeleteProgram(program);
    return 0;
}

// Framebuffer widgets share Cardinal's GL context. Restore modified state
// before the rack draws any other panel.
struct ShaderDrawState {
    static constexpr GLenum capabilities[6] = {
        GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_SCISSOR_TEST, GL_STENCIL_TEST, GL_DITHER
    };
    GLboolean enabled[6];
    GLint viewport[4];
    GLboolean colorMask[4];
    GLfloat clearColor[4];
    GLint program, buffer, attributeBuffer, attributeEnabled;
    GLint size, type, normalized, stride;
    void* pointer = nullptr;

    ShaderDrawState() {
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attributeBuffer);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attributeEnabled);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &normalized);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
        glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
        for (unsigned i = 0; i < 6; ++i) {
            enabled[i] = glIsEnabled(capabilities[i]);
            glDisable(capabilities[i]);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }

    ~ShaderDrawState() {
        glUseProgram(program);
        if (attributeBuffer) {
            glBindBuffer(GL_ARRAY_BUFFER, attributeBuffer);
            glVertexAttribPointer(0, size, type, normalized, stride, pointer);
        }
        if (attributeEnabled)
            glEnableVertexAttribArray(0);
        else
            glDisableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        for (unsigned i = 0; i < 6; ++i)
            if (enabled[i])
                glEnable(capabilities[i]);
        glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }
};

constexpr GLenum ShaderDrawState::capabilities[6];

struct ShaderPreview : widget::OpenGlWidget {
    ShaderModule* const module;
    GLuint program = 0, soundProgram = 0;
    GLuint buffer = 0, audioFramebuffer = 0, audioTexture = 0, feedback = 0, emptyTexture = 0;
    int feedbackWidth = 0, feedbackHeight = 0;
    bool wantsFeedback = false;
    bool wantsMrt = false;
    GLuint mrtFramebuffer = 0;
    GLuint mrtTextures[2] {};
    GLuint secondFeedback = 0;
    double started = 0.0;
    double lastFrame = -1.0;
    double frameDelta = 0.0;
    unsigned frame = 0;
    uint64_t sampleIndex = 0;
    double soundTime = 0.0;
    bool applyPending = false;
    bool stopPending = false;
    bool accepted = false;
    bool pointerDown = false;
    bool pointerClicked = false;
    Vec clickPosition;
    std::string candidate;
    std::string activeSource;
    std::string error;

    explicit ShaderPreview(ShaderModule* owner) : module(owner) {}

    ~ShaderPreview() override { releaseResources(); }

    virtual bool running() const { return program || soundProgram; }
    virtual bool busy() const { return applyPending || running(); }

    virtual void submit(std::string source) {
        candidate = std::move(source);
        error.clear();
        applyPending = true;
        stopPending = false;
        accepted = false;
    }

    virtual void stop() {
        module->soundRunning.store(false);
        ++module->soundGeneration;
        applyPending = false;
        stopPending = true;
        accepted = false;
        error.clear();
    }

    void releasePrograms() {
        module->soundRunning.store(false);
        ++module->soundGeneration;
        if (program)
            glDeleteProgram(program);
        if (soundProgram)
            glDeleteProgram(soundProgram);
        program = soundProgram = 0;
        if (feedback)
            glDeleteTextures(1, &feedback);
        if (secondFeedback)
            glDeleteTextures(1, &secondFeedback);
        if (mrtFramebuffer)
            glDeleteFramebuffers(1, &mrtFramebuffer);
        glDeleteTextures(2, mrtTextures);
        mrtTextures[0] = mrtTextures[1] = 0;
        feedback = secondFeedback = mrtFramebuffer = 0;
        feedbackWidth = feedbackHeight = 0;
    }

    void releaseResources() {
        releasePrograms();
        if (buffer)
            glDeleteBuffers(1, &buffer);
        if (audioFramebuffer)
            glDeleteFramebuffers(1, &audioFramebuffer);
        if (audioTexture)
            glDeleteTextures(1, &audioTexture);
        if (emptyTexture)
            glDeleteTextures(1, &emptyTexture);
        buffer = audioFramebuffer = audioTexture = emptyTexture = 0;
    }

    void onContextDestroy(const ContextDestroyEvent& e) override {
        releaseResources();
        if (!applyPending && !stopPending && !activeSource.empty())
            submit(activeSource);
        OpenGlWidget::onContextDestroy(e);
    }

    void step() override {
        if (applyPending || stopPending || running())
            setDirty();
        FramebufferWidget::step();
        if (!stopPending && !applyPending && soundProgram && module->sound.size() < 4096) {
            ShaderDrawState restore;
            GLint previousUnit, previousTexture;
            glGetIntegerv(GL_ACTIVE_TEXTURE, &previousUnit);
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
            glBindTexture(GL_TEXTURE_2D, emptyTexture);
            bindGeometry();
            renderAudio(module->sampleRate.load(std::memory_order_relaxed));
            glBindTexture(GL_TEXTURE_2D, previousTexture);
            glActiveTexture(previousUnit);
        }
    }

    void draw(const DrawArgs&) override {}

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1 && !args.fb)
            OpenGlWidget::draw(args);
        OpenGlWidget::drawLayer(args, layer);
    }

    bool compileCandidate() {
        const auto source = cardinalShader::withoutComments(candidate);
        const bool hasAudio = cardinalShader::hasSound(source);
        const bool hasVisual = !hasAudio || cardinalShader::hasImage(source) || cardinalShader::hasMain(source);
        GLuint image = 0;
        GLuint sound = 0;
        if (hasVisual) {
            image = linkShader(cardinalShader::prepare(candidate, source, false), error);
            if (!image)
                return false;
        }
        if (hasAudio) {
            sound = linkShader(cardinalShader::prepare(candidate, source, true), error);
            if (!sound) {
                if (image)
                    glDeleteProgram(image);
                return false;
            }
        }
        releasePrograms();
        program = image;
        soundProgram = sound;
        module->soundRunning.store(sound != 0);
        wantsMrt = !cardinalShader::hasImage(source) && cardinalShader::usesMrt(source);
        wantsFeedback = cardinalShader::usesFeedback(source) || wantsMrt;
        frame = 0;
        sampleIndex = 0;
        soundTime = 0.0;
        started = lastFrame;
        activeSource = candidate;
        accepted = true;
        return true;
    }

    void bindGeometry() {
        if (!buffer) {
            const GLfloat vertices[] = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};
            glGenBuffers(1, &buffer);
            glBindBuffer(GL_ARRAY_BUFFER, buffer);
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        }
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }

    Vec pointerPosition() {
        const auto origin = getAbsoluteOffset(Vec());
        const Vec scale = getAbsoluteOffset(Vec(1.f, 1.f)).minus(origin);
        const Vec offset = APP->scene->getMousePos().minus(origin);
        return Vec(math::clamp(offset.x / scale.x / box.size.x, 0.f, 1.f),
                   1.f - math::clamp(offset.y / scale.y / box.size.y, 0.f, 1.f));
    }

    void bindUniforms(GLuint target, Vec size, float elapsed, float rate) {
        auto uniform = [target](const char* name) { return glGetUniformLocation(target, name); };
        glUniform2f(uniform("u_resolution"), size.x, size.y);
        glUniform3f(uniform("iResolution"), size.x, size.y, 1.f);
        glUniform1f(uniform("u_time"), elapsed);
        glUniform1f(uniform("iTime"), elapsed);
        glUniform1f(uniform("iTimeDelta"), float(frameDelta));
        glUniform1f(uniform("iSampleRate"), rate);
        glUniform1i(uniform("iFrame"), static_cast<GLint>(frame));
        if (const GLint location = uniform("iDate"); location >= 0) {
            const std::time_t seconds = std::time(nullptr);
            if (const auto* date = std::localtime(&seconds))
                glUniform4f(location, date->tm_year + 1900, date->tm_mon + 1, date->tm_mday,
                            date->tm_hour * 3600 + date->tm_min * 60 + date->tm_sec);
        }
        if (const GLint mouse = uniform("iMouse"); mouse >= 0) {
            const Vec pointer = pointerPosition();
            const float sign = pointerDown ? 1.f : -1.f;
            glUniform4f(mouse, pointer.x * size.x, pointer.y * size.y,
                        pointerClicked ? sign * clickPosition.x * size.x : 0.f,
                        pointerClicked ? sign * clickPosition.y * size.y : 0.f);
        }

        GLfloat volts[maxCV] {};
        for (int i = 0; i < module->inputCount; ++i)
            volts[i] = module->cv[i].load(std::memory_order_relaxed);
        GLint uniforms = 0;
        glGetProgramiv(target, GL_ACTIVE_UNIFORMS, &uniforms);
        for (GLint i = 0; i < uniforms; ++i) {
            char name[64] {};
            GLenum type = 0;
            GLint count = 0;
            glGetActiveUniform(target, i, sizeof(name), nullptr, &count, &type, name);
            if (std::strcmp(name, "u_cv") == 0) {
                const GLint cv = uniform(name);
                if (type == GL_FLOAT_VEC2)
                    glUniform2fv(cv, 1, volts);
                else if (type == GL_FLOAT_VEC3)
                    glUniform3fv(cv, 1, volts);
                else if (type == GL_FLOAT_VEC4)
                    glUniform4fv(cv, 1, volts);
            } else if (std::strcmp(name, "u_inputs[0]") == 0 && type == GL_FLOAT) {
                glUniform1fv(uniform(name), std::min(count, maxCV), volts);
            } else if (std::strcmp(name, "iChannelResolution[0]") == 0 && type == GL_FLOAT_VEC3) {
                const GLfloat sizes[12] = {wantsFeedback ? size.x : 1.f, wantsFeedback ? size.y : 1.f, 1.f,
                                           1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f};
                glUniform3fv(uniform(name), std::min(count, 4), sizes);
            }
        }
    }

    static void allocateTexture(GLuint& texture, int width, int height, const void* pixels = nullptr) {
        if (!texture)
            glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }

    void renderAudio(float rate) {
        constexpr int width = 256, height = 4, samples = width * height;
        if (!soundProgram || rate <= 0.f || module->soundReady.load() != module->soundGeneration.load()
            || module->sound.size() >= 4096)
            return;
        GLint drawFramebuffer, readFramebuffer;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
        if (!audioFramebuffer) {
            allocateTexture(audioTexture, width, height);
            glGenFramebuffers(1, &audioFramebuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, audioFramebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, audioTexture, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
                glDeleteFramebuffers(1, &audioFramebuffer);
                audioFramebuffer = 0;
                stop();
                error = "Could not create the GPU audio buffer.";
                return;
            }
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, audioFramebuffer);
        }

        glUseProgram(soundProgram);
        glViewport(0, 0, width, height);
        glBindTexture(GL_TEXTURE_2D, emptyTexture);
        bindUniforms(soundProgram, Vec(width, height), float(soundTime), rate);
        const GLint timeBase = glGetUniformLocation(soundProgram, "cardinalTimeBase");
        const GLint sampleBase = glGetUniformLocation(soundProgram, "cardinalSampleBase");
        std::array<GLubyte, samples * 4> bytes;
        std::array<ShaderModule::Stereo, samples> block;
        for (int i = 0; i < 4 && module->sound.size() < 4096; ++i) {
            glClearColor(0.f, 128.f / 255.f, 0.f, 128.f / 255.f);
            glClear(GL_COLOR_BUFFER_BIT);
            glUniform1f(timeBase, float(soundTime));
            glUniform1i(sampleBase, static_cast<GLint>(sampleIndex & 0x7fffffff));
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
            for (int sample = 0; sample < samples; ++sample) {
                const int offset = sample * 4;
                const float left = (bytes[offset] + 256 * bytes[offset + 1] - 32768) / 32767.f;
                const float right = (bytes[offset + 2] + 256 * bytes[offset + 3] - 32768) / 32767.f;
                block[sample] = {left, right};
            }
            module->sound.pushBuffer(block.data(), samples);
            sampleIndex += samples;
            soundTime += samples / double(rate);
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
    }

    void renderImage(Vec size, float rate) {
        GLint drawFramebuffer, readFramebuffer;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
        if (wantsFeedback && (feedbackWidth != int(size.x) || feedbackHeight != int(size.y))) {
            feedbackWidth = size.x;
            feedbackHeight = size.y;
            allocateTexture(feedback, feedbackWidth, feedbackHeight);
            if (wantsMrt) {
                if (!mrtFramebuffer)
                    glGenFramebuffers(1, &mrtFramebuffer);
                glBindFramebuffer(GL_FRAMEBUFFER, mrtFramebuffer);
                constexpr GLenum attachments[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
                for (int i = 0; i < 2; ++i) {
                    allocateTexture(mrtTextures[i], feedbackWidth, feedbackHeight);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, attachments[i], GL_TEXTURE_2D, mrtTextures[i], 0);
                }
                glDrawBuffers(2, attachments);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                    error = "Could not create the two shader render targets.";
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
                    return;
                }
                glActiveTexture(GL_TEXTURE1);
                allocateTexture(secondFeedback, feedbackWidth, feedbackHeight);
                glActiveTexture(GL_TEXTURE0);
            }
        }
        if (mrtFramebuffer) {
            glBindFramebuffer(GL_FRAMEBUFFER, mrtFramebuffer);
            glClear(GL_COLOR_BUFFER_BIT);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, secondFeedback);
            glActiveTexture(GL_TEXTURE0);
        }
        if (feedback)
            glBindTexture(GL_TEXTURE_2D, feedback);
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program, "iChannel1"), 1);
        glUniform1i(glGetUniformLocation(program, "iChannel2"), 2);
        glUniform1i(glGetUniformLocation(program, "iChannel3"), 2);
        bindUniforms(program, size, float(lastFrame - started), rate);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        if (mrtFramebuffer)
            glReadBuffer(GL_COLOR_ATTACHMENT0);
        if (feedback)
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, feedbackWidth, feedbackHeight);
        if (mrtFramebuffer) {
            glReadBuffer(GL_COLOR_ATTACHMENT1);
            glActiveTexture(GL_TEXTURE1);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, feedbackWidth, feedbackHeight);
            glActiveTexture(GL_TEXTURE0);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer);
            glBlitFramebuffer(0, 0, feedbackWidth, feedbackHeight, 0, 0, feedbackWidth, feedbackHeight,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
        }
    }

    void drawFramebuffer() override {
        const double now = system::getTime();
        frameDelta = lastFrame < 0.0 ? 0.0 : now - lastFrame;
        lastFrame = now;
        if (stopPending) {
            releasePrograms();
            activeSource.clear();
            stopPending = false;
        }
        if (applyPending) {
            applyPending = false;
            compileCandidate();
        }

        ShaderDrawState restore;
        const auto size = getFramebufferSize();
        glViewport(0, 0, size.x, size.y);
        glClearColor(.015f, .02f, .03f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!running())
            return;

        GLint previousUnit, previousTextures[3];
        glGetIntegerv(GL_ACTIVE_TEXTURE, &previousUnit);
        for (int i = 0; i < 3; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTextures[i]);
        }
        if (!emptyTexture) {
            const GLubyte black[] = {0, 0, 0, 255};
            allocateTexture(emptyTexture, 1, 1, black);
        }
        glBindTexture(GL_TEXTURE_2D, emptyTexture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, emptyTexture);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, emptyTexture);
        bindGeometry();
        const float rate = module->sampleRate.load(std::memory_order_relaxed);
        if (program)
            renderImage(size, rate);
        ++frame;
        for (int i = 0; i < 3; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, previousTextures[i]);
        }
        glActiveTexture(previousUnit);
    }
};

// Browser-rendered visuals use the same layer as GLSL. A sandboxed frame
// transfers a finished bitmap; the rack keeps ownership of the GL context.
struct ScriptPreview : ShaderPreview, app::NavigationSignal {
    bool drawsPatchSignal() const override {
        if (!startedScript)
            return false;
        for (const auto& output : module->outputs) {
            if (output.isConnected())
                return true;
        }
        return false;
    }
    const unsigned runner = ++nextRunner;
    GLuint frameTexture = 0, displayProgram = 0;
    bool startedScript = false;
    bool hasFrame = false;
    bool scriptRunning = false;

    explicit ScriptPreview(ShaderModule* owner) : ShaderPreview(owner) {}

    ~ScriptPreview() override {
        stop();
        releaseDisplay();
    }

    bool running() const override { return scriptRunning; }
    bool busy() const override { return applyPending || startedScript; }

    void stop() override {
        EM_ASM({ if (Module.cardinalCode) Module.cardinalCode.removeVisual($0); }, runner);
        applyPending = startedScript = scriptRunning = hasFrame = accepted = false;
        stopPending = true;
        error.clear();
        for (auto& output : module->visualOutput)
            output.store(0.f, std::memory_order_relaxed);
        setDirty();
    }

    void releaseDisplay() {
        if (frameTexture)
            glDeleteTextures(1, &frameTexture);
        if (displayProgram)
            glDeleteProgram(displayProgram);
        frameTexture = displayProgram = 0;
        hasFrame = false;
    }

    void onContextDestroy(const ContextDestroyEvent& e) override {
        releaseDisplay();
        ShaderPreview::onContextDestroy(e);
    }

    void step() override {
        if (applyPending || stopPending || startedScript)
            setDirty();
        FramebufferWidget::step();
    }

    void drawFramebuffer() override {
        stopPending = false;
        if (applyPending) {
            startedScript = EM_ASM_INT({
                if (!Module.cardinalCode) return 0;
                Module.cardinalCode.runVisual($0, $1, UTF8ToString($2));
                return 1;
            }, runner, static_cast<int>(module->runtime), candidate.c_str()) != 0;
            if (startedScript) {
                applyPending = false;
                scriptRunning = false;
            }
        }

        ShaderDrawState restore;
        glDisable(GL_DITHER);
        const auto size = getFramebufferSize();
        glViewport(0, 0, size.x, size.y);
        glClearColor(.015f, .02f, .03f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!startedScript)
            return;
        if (!displayProgram)
            displayProgram = linkShader(
                "#version 300 es\nprecision highp float;\nin vec2 v_uv;\n"
                "uniform sampler2D frame; out vec4 color;\n"
                "void main() { color = texture(frame, vec2(v_uv.x, 1. - v_uv.y)); }", error);
        if (!displayProgram)
            return;

        GLint previousUnit, previousTexture;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &previousUnit);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        if (!frameTexture)
            allocateTexture(frameTexture, 1, 1);
        glBindTexture(GL_TEXTURE_2D, frameTexture);
        float values[maxCV] {};
        for (int i = 0; i < module->inputCount; ++i)
            values[i] = module->cv[i].load(std::memory_order_relaxed);
        char message[512] {};
        const Vec pointer = pointerPosition();
        const int result = EM_ASM_INT({
            const input = Array.from(HEAPF32.subarray($1 >> 2, ($1 >> 2) + $2));
            const result = Module.cardinalCode.updateVisual($0, input, $3, $4, $5, $6, $7);
            if (result.error) stringToUTF8(result.error, $8, $9);
            HEAPF32.subarray($1 >> 2, ($1 >> 2) + 6).fill(0);
            HEAPF32.set(result.output, $1 >> 2);
            if (!result.bitmap) return result.error ? -1 : 0;
            Module.cardinalCode.uploadBitmap(GLctx, result.bitmap);
            return 1;
        }, runner, values, module->inputCount, box.size.x, box.size.y, pointer.x, 1.f - pointer.y,
           pointerDown, message, sizeof(message));
        if (result < 0) {
            error = message;
            startedScript = scriptRunning = false;
            hasFrame = false;
        } else if (result > 0) {
            if (!scriptRunning) {
                activeSource = candidate;
                accepted = true;
            }
            hasFrame = scriptRunning = true;
        }
        for (int i = 0; i < maxCV; ++i)
            module->visualOutput[i].store(i < module->outputCount ? values[i] : 0.f, std::memory_order_relaxed);

        if (hasFrame) {
            bindGeometry();
            glUseProgram(displayProgram);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glBindTexture(GL_TEXTURE_2D, previousTexture);
        glActiveTexture(previousUnit);
    }
};

struct StrudelPreview : ShaderPreview {
    const unsigned runner = ++nextRunner;
    bool started = false;
    bool playing = false;
    int sourceRevision = 0;
    std::string transportMessage;

    GLuint texture = 0, display = 0;
    bool hasBitmap = false;
    std::array<TextEditor::PlaybackMark, 128> marks;
    int markCount = 0;
    std::string visualError;

    explicit StrudelPreview(ShaderModule* owner) : ShaderPreview(owner) {}
    ~StrudelPreview() override {
        stop();
        releaseDisplay();
    }

    void releaseDisplay() {
        if (texture)
            glDeleteTextures(1, &texture);
        if (display)
            glDeleteProgram(display);
        texture = display = 0;
        hasBitmap = false;
    }

    void onContextDestroy(const ContextDestroyEvent& e) override {
        releaseDisplay();
        OpenGlWidget::onContextDestroy(e);
    }

    bool running() const override { return playing; }
    bool busy() const override { return applyPending || started; }

    void stop() override {
        ShaderPreview::stop();
        module->strudel->runner.store(0);
        EM_ASM({ if (Module.cardinalCode) Module.cardinalCode.removeStrudel($0); }, runner);
        started = playing = hasBitmap = false;
        sourceRevision = 0;
        transportMessage.clear();
        markCount = 0;
        visualError.clear();
        setDirty();
    }

    void requestStop() {
        const int queued = EM_ASM_INT({
            return Module.cardinalCode && Module.cardinalCode.requestStrudelStop($0) ? 1 : 0;
        }, runner);
        if (!queued)
            stop();
        else
            applyPending = false;
    }

    void step() override {
        FramebufferWidget::step();
        const float rate = module->sampleRate.load(std::memory_order_relaxed);
        if (rate <= 0.f)
            return;
        if (applyPending && started && module->followBrowserTransport && rack::browserTransport().enabled) {
            const bool queued = EM_ASM_INT({
                return Module.cardinalCode.queueStrudelUpdate($0, UTF8ToString($1)) ? 1 : 0;
            }, runner, candidate.c_str()) != 0;
            if (queued)
                applyPending = false;
        }
        if (applyPending) {
            module->strudel->runner.store(0);
            playing = hasBitmap = false;
            sourceRevision = 0;
            transportMessage.clear();
            markCount = 0;
            visualError.clear();
            setDirty();
            started = EM_ASM_INT({
                if (!Module.cardinalCode) return 0;
                Module.cardinalCode.runStrudel($0, UTF8ToString($1), $2, !!$3);
                return 1;
            }, runner, candidate.c_str(), rate,
               module->followBrowserTransport && rack::browserTransport().enabled) != 0;
            applyPending = !started;
            if (started)
                module->strudel->runner.store(runner);
        }
        if (!started)
            return;
        char message[512] {};
        char* appliedSource = nullptr;
        const int status = EM_ASM_INT({
            const result = Module.cardinalCode.strudelStatus($0);
            if (result.error) {
                stringToUTF8(result.error, $1, $2);
                return -1;
            }
            if (result.waitingForGesture) return -2;
            if (result.finished) return -3;
            if (result.pending) stringToUTF8(result.pending, $1, $2);
            if (result.revision && result.revision !== $3) {
                const size = lengthBytesUTF8(result.source) + 1;
                const buffer = _malloc(size);
                if (buffer) {
                    stringToUTF8(result.source, buffer, size);
                    HEAPU32[$4 >> 2] = buffer;
                }
            }
            return ((result.revision || 0) << 1) | (result.running ? 1 : 0);
        }, runner, message, sizeof(message), sourceRevision, &appliedSource);
        if (status == -1) {
            stop();
            error = message;
        } else if (status == -2) {
            error = "Click the rack to enable audio.";
        } else if (status == -3) {
            stop();
        } else {
            playing = (status & 1) != 0;
            transportMessage = message;
            if (appliedSource) {
                error.clear();
                activeSource = appliedSource;
                std::free(appliedSource);
                sourceRevision = status >> 1;
                accepted = true;
            }
        }
        if (!started || app::rackNavigation() == app::RackNavigation::Moving)
            return;

        static_assert(sizeof(TextEditor::PlaybackMark) == 8 * sizeof(uint32_t), "Strudel mark packing");
        int bitmapReady = 0;
        char visualMessage[512] {};
        markCount = EM_ASM_INT({
            const feedback = Module.cardinalCode.updateStrudelVisual($0, $1, $2);
            if (feedback.error) stringToUTF8(feedback.error, $6, $7);
            HEAP32[$5 >> 2] = feedback.bitmapReady ? 1 : 0;
            if (!feedback.marks) return 0;
            const marks = feedback.marks.subarray(0, $4 * 8);
            HEAPU32.set(marks, $3 >> 2);
            return marks.length / 8;
        }, runner, box.size.x, box.size.y, marks.data(), marks.size(), &bitmapReady,
           visualMessage, sizeof(visualMessage));
        visualError = visualMessage;
        if (bitmapReady)
            setDirty();
    }

    void drawFramebuffer() override {
        ShaderDrawState restore;
        const auto size = getFramebufferSize();
        glViewport(0, 0, size.x, size.y);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!started)
            return;
        if (!display)
            display = linkShader(
                "#version 300 es\nprecision highp float;\nin vec2 v_uv;\n"
                "uniform sampler2D frame; out vec4 color;\n"
                "void main() { color = texture(frame, vec2(v_uv.x, 1. - v_uv.y)); }", visualError);
        if (!display)
            return;

        GLint previousUnit, previousTexture;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &previousUnit);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        if (!texture)
            allocateTexture(texture, 1, 1);
        glBindTexture(GL_TEXTURE_2D, texture);
        const int uploaded = EM_ASM_INT({
            const bitmap = Module.cardinalCode.takeStrudelBitmap($0);
            if (!bitmap) return 0;
            Module.cardinalCode.uploadBitmap(GLctx, bitmap);
            return 1;
        }, runner);
        hasBitmap = hasBitmap || uploaded;
        if (hasBitmap) {
            bindGeometry();
            glUseProgram(display);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glBindTexture(GL_TEXTURE_2D, previousTexture);
        glActiveTexture(previousUnit);
    }
};

struct ShaderWidget : ModuleWidget {
    ShaderModule* const shader;
    CodeEditor* editor;
    ShaderPreview* preview = nullptr;
    CodeButton* runButton;
    app::PortWidget* inputJacks[maxCV] {};
    app::PortWidget* outputJacks[maxCV] {};
    const VisualRuntime runtime;
    CodeButton* layoutButton = nullptr;
    ShaderLayout shownLayout = ShaderLayout::SideBySide;
    unsigned revision;
    std::string message;

    explicit ShaderWidget(ShaderModule* module, VisualRuntime kind = VisualRuntime::GLSL)
        : shader(module), runtime(kind), revision(shader ? shader->revision : 0) {
        setModule(module);
        const bool strudel = runtime == VisualRuntime::Strudel;
        box.size = Vec((strudel ? 35 : 52) * RACK_GRID_WIDTH, RACK_GRID_HEIGHT);
        for (float x : {RACK_GRID_WIDTH, box.size.x - 2 * RACK_GRID_WIDTH}) {
            addChild(createWidget<ThemedScrew>(Vec(x, 0)));
            addChild(createWidget<ThemedScrew>(Vec(x, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        }

        if (shader) {
            if (strudel)
                preview = new StrudelPreview(shader);
            else if (runtime == VisualRuntime::GLSL)
                preview = new ShaderPreview(shader);
            else
                preview = new ScriptPreview(shader);
            addChild(preview);
        }
        editor = new CodeEditor(++nextRunner, runtime == VisualRuntime::GLSL ? "GLSL" : "JavaScript");
        editor->setText(shader ? shader->source : visualSource(runtime));
        addChild(editor);

        editor->runAction = [this] { apply(); };
        editor->stopAction = [this] { stopPlayback(); };
        runButton = addButton("▶", 48, 55, [this] {
            if (preview && preview->busy())
                stopPlayback();
            else
                apply();
        });
        layoutButton = addButton("Side by side", 109, 110, [this] {
            if (!shader)
                return;
            auto* menu = createMenu();
            menu->box.pos = layoutButton->getAbsoluteOffset(Vec(0, layoutButton->box.size.y));
            appendLayouts(menu);
        });
        const int inputChannels = strudel ? int(StrudelAudio::InputCount) : maxCV;
        for (int i = 0; i < inputChannels; ++i) {
            inputJacks[i] = createInputCentered<PJ301MPort>(
                Vec(jackColumnWidth * .5f, firstJackY + i * jackSpacing), module, i);
            addInput(inputJacks[i]);
        }
        const int channels = strudel ? strudelChannels : hasStereoAudio(runtime) ? 2 : maxCV;
        for (int i = 0; i < channels; ++i) {
            outputJacks[i] = createOutputCentered<PJ301MPort>(
                Vec(box.size.x - jackColumnWidth * .5f, firstJackY + jackSpacing * i), module, i);
            addOutput(outputJacks[i]);
        }
        layout();
    }

    CodeButton* addButton(const char* label, float x, float width, std::function<void()> action) {
        auto* button = new CodeButton;
        button->text = label;
        button->box = Rect(Vec(x, 8), Vec(width, 22));
        button->action = std::move(action);
        addChild(button);
        return button;
    }

    void layout() {
        shownLayout = shader ? shader->layout
            : runtime == VisualRuntime::Strudel ? ShaderLayout::Overlay : ShaderLayout::SideBySide;
        editor->visible = shownLayout != ShaderLayout::PreviewOnly;
        layoutButton->text = shaderLayouts[static_cast<int>(shownLayout)];
        const float contentWidth = box.size.x - 2 * jackColumnWidth;
        const float splitWidth = (contentWidth - 8) * .5f;
        const bool split = shownLayout == ShaderLayout::SideBySide;
        editor->box = Rect(Vec(jackColumnWidth, 35), Vec(split ? splitWidth : contentWidth, 320));
        if (preview)
            preview->box = Rect(Vec(split ? jackColumnWidth + splitWidth + 8 : jackColumnWidth, 35),
                                Vec(split ? splitWidth : contentWidth, 320));
    }

    void appendLayouts(ui::Menu* menu) {
        for (int i = 0; i < 3; ++i) {
            const auto mode = static_cast<ShaderLayout>(i);
            menu->addChild(createCheckMenuItem(shaderLayouts[i], "",
                [this, mode] { return shader->layout == mode; },
                [this, mode] { shader->layout = mode; layout(); }));
        }
    }

    void stopPlayback() {
        if (!preview)
            return;
        if (runtime == VisualRuntime::Strudel)
            static_cast<StrudelPreview*>(preview)->requestStop();
        else
            preview->stop();
    }

    void apply() {
        if (!preview)
            return;
        auto source = editor->getText();
        if (!source.empty() && source.back() == '\n')
            source.pop_back();
        if (source.size() > maxSourceSize || source.find('\0') != std::string::npos) {
            message = "Source must be at most 16 KiB and contain no NUL characters.";
            return;
        }
        message.clear();
        preview->submit(std::move(source));
    }

    void onButton(const ButtonEvent& e) override {
        if (preview && e.button == GLFW_MOUSE_BUTTON_LEFT) {
            if (e.action == GLFW_RELEASE)
                preview->pointerDown = false;
            else if (e.action == GLFW_PRESS && preview->box.contains(e.pos)) {
                preview->pointerDown = preview->pointerClicked = true;
                preview->clickPosition = preview->pointerPosition();
            }
        }
        ModuleWidget::onButton(e);
    }

    void onHoverKey(const HoverKeyEvent& e) override {
        if (handleRunShortcut(e, editor->runAction, editor->stopAction))
            e.consume(this);
        else
            ModuleWidget::onHoverKey(e);
    }

    void appendContextMenu(ui::Menu* menu) override {
        menu->addChild(createSubmenuItem("Editor", "", [this](ui::Menu* submenu) {
            editor->appendAppearanceMenu(submenu);
        }));
        if (shader && shader->strudel) {
            menu->addChild(createCheckMenuItem("Follow Browser Transport", "",
                [this] { return shader->followBrowserTransport; },
                [this] {
                    shader->followBrowserTransport = !shader->followBrowserTransport;
                    preview->stop();
                }));
            menu->addChild(createSubmenuItem("Clock pulses per beat",
                std::to_string(shader->strudel->pulsesPerBeat.load()), [this](ui::Menu* list) {
                    for (const int ppqn : {1, 2, 4, 8, 12, 24, 48}) {
                        list->addChild(createCheckMenuItem(std::to_string(ppqn), "",
                            [this, ppqn] { return shader->strudel->pulsesPerBeat.load() == ppqn; },
                            [this, ppqn] { shader->strudel->pulsesPerBeat.store(ppqn); }));
                    }
                }));
            menu->addChild(createMenuItem("Enable MIDI output", "", [this] {
                EM_ASM({ if (Module.cardinalCode) Module.cardinalCode.enableStrudelMidi(); });
            }));
        }
        if (shader && runtime != VisualRuntime::Strudel) {
            addJackCountMenu(menu, "Inputs", this, true, shader->inputCount);
            if (!hasStereoAudio(runtime))
                addJackCountMenu(menu, "Outputs", this, false, shader->outputCount);
        }
    }

    void step() override {
        for (int i = 0; i < maxCV && inputJacks[i]; ++i)
            inputJacks[i]->visible = runtime == VisualRuntime::Strudel || i < (shader ? shader->inputCount : defaultCV)
                || (shader && shader->inputs[i].isConnected());
        if (!hasStereoAudio(runtime)) {
            for (int i = 0; i < maxCV; ++i)
                outputJacks[i]->visible = i < (shader ? shader->outputCount : defaultCV)
                    || (shader && shader->outputs[i].isConnected());
        }
        if (preview) {
            if (revision != shader->revision) {
                editor->setText(shader->source);
                if (runtime != VisualRuntime::Strudel || preview->busy())
                    preview->submit(shader->source);
                revision = shader->revision;
            }
            if (shownLayout != shader->layout)
                layout();
            if (preview->accepted) {
                preview->accepted = false;
                if (preview->activeSource != shader->source) {
                    auto* action = new history::ModuleChange;
                    action->name = runtime == VisualRuntime::GLSL ? "edit GLSL shader" : "edit visual code";
                    action->moduleId = shader->id;
                    action->oldModuleJ = toJson();
                    shader->source = preview->activeSource;
                    action->newModuleJ = toJson();
                    APP->history->push(action);
                }
            }
        }
        if (preview && runtime == VisualRuntime::Strudel) {
            auto* strudel = static_cast<StrudelPreview*>(preview);
            auto text = editor->getText();
            if (!text.empty() && text.back() == '\n')
                text.pop_back();
            if (strudel->running() && strudel->markCount && text == strudel->activeSource)
                editor->setPlaybackMarks(strudel->marks.data(), strudel->markCount);
            else
                editor->setPlaybackMarks();
        }
        ModuleWidget::step();
    }

    void draw(const DrawArgs& args) override {
        drawCodePanel(args.vg, box.size);
        if (runtime == VisualRuntime::Strudel) {
            const char* labels[] = {"CLK", "L IN", "R IN"};
            for (int i = 0; i < StrudelAudio::InputCount; ++i)
                nvgText(args.vg, jackColumnWidth * .5f, firstJackY + i * jackSpacing - 18, labels[i], nullptr);
        } else {
            drawJackColumn(args.vg, jackColumnWidth * .5f, "IN", shader ? shader->inputCount : defaultCV, false);
        }
        drawJackColumn(args.vg, box.size.x - jackColumnWidth * .5f, "OUT",
                       runtime == VisualRuntime::Strudel ? strudelChannels
                           : hasStereoAudio(runtime) ? 2 : shader ? shader->outputCount : defaultCV, true);
        runButton->lit = preview && preview->running();
        if (preview) {
            nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            const auto& error = !editor->formatError.empty() ? editor->formatError
                : message.empty() ? preview->error : message;
            std::string status = error;
            if (status.empty() && runtime == VisualRuntime::Strudel) {
                const auto* strudel = static_cast<StrudelPreview*>(preview);
                status = strudel->visualError.empty() ? strudel->transportMessage : strudel->visualError;
            }
            if (!status.empty()) {
                nvgFillColor(args.vg, nvgRGB(115, 105, 103));
                nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
                nvgSave(args.vg);
                const float width = box.size.x - 2 * jackColumnWidth;
                nvgIntersectScissor(args.vg, jackColumnWidth, 355, width, 19);
                nvgTextBox(args.vg, jackColumnWidth, 356, width, status.c_str(), nullptr);
                nvgRestore(args.vg);
            }
        }
        ModuleWidget::draw(args);
    }
};

template <VisualRuntime Kind>
struct VisualModule : ShaderModule {
    VisualModule() : ShaderModule(Kind) {}
};

template <VisualRuntime Kind>
struct VisualWidget : ShaderWidget {
    explicit VisualWidget(VisualModule<Kind>* module) : ShaderWidget(module, Kind) {}
};

} // namespace

Model* modelCardinalP5 = createModel<VisualModule<VisualRuntime::P5>, VisualWidget<VisualRuntime::P5>>("P5");
Model* modelCardinalThree = createModel<VisualModule<VisualRuntime::Three>, VisualWidget<VisualRuntime::Three>>("Three");
Model* modelCardinalAscii = createModel<VisualModule<VisualRuntime::ASCII>, VisualWidget<VisualRuntime::ASCII>>("PlayCore");
Model* modelCardinalStrudel = createModel<VisualModule<VisualRuntime::Strudel>, VisualWidget<VisualRuntime::Strudel>>("Strudel");
Model* modelCardinalShader = createModel<ShaderModule, ShaderWidget>("Shader");
Model* modelCardinalCode = createModel<CodeModule, CodeWidget>("Code");
#include "LivecodeKeys.hpp"
#endif
