// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin.hpp"

#if defined(__EMSCRIPTEN__) && !defined(HEADLESS)
#include "ImGuiTextEditor.hpp"

#include <GLES2/gl2.h>

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
    explicit CodeEditor(unsigned id, const char* language = "JavaScript") : runner(id) {
        setShowStatus(false);
        setLanguageDefinition(language);
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

struct ShaderModule : Module {
    std::atomic<float> cv[2] {{0.f}, {0.f}};
    std::string source = defaultShader;
    unsigned revision = 0;
    bool previewOnly = false;

    ShaderModule() {
        config(0, 2, 0, 0);
        configInput(0, "Shader CV 1");
        configInput(1, "Shader CV 2");
    }

    void process(const ProcessArgs&) override {
        for (int i = 0; i < 2; ++i)
            cv[i].store(inputs[i].getVoltage(), std::memory_order_relaxed);
    }

    json_t* dataToJson() override {
        return json_pack("{s:s,s:b}", "source", source.c_str(), "previewOnly", previewOnly);
    }

    void dataFromJson(json_t* data) override {
        const json_t* text = json_object_get(data, "source");
        const char* loaded = json_is_string(text) && json_string_length(text) <= maxSourceSize
            ? json_string_value(text) : defaultShader;
        if (source != loaded) {
            source = loaded;
            ++revision;
        }
        previewOnly = json_is_true(json_object_get(data, "previewOnly"));
    }

    void onReset(const ResetEvent& e) override {
        Module::onReset(e);
        source = defaultShader;
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
    const GLuint vertex = compileShader(GL_VERTEX_SHADER,
        "attribute vec2 a_position;\n"
        "void main() { gl_Position = vec4(a_position, 0.0, 1.0); }", error);
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
    static constexpr GLenum capabilities[5] = {
        GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_SCISSOR_TEST, GL_STENCIL_TEST
    };
    GLboolean enabled[5];
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
        for (unsigned i = 0; i < 5; ++i) {
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
        for (unsigned i = 0; i < 5; ++i)
            if (enabled[i])
                glEnable(capabilities[i]);
        glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }
};

constexpr GLenum ShaderDrawState::capabilities[5];

struct ShaderPreview : widget::OpenGlWidget {
    ShaderModule* const module;
    GLuint program = 0;
    GLuint buffer = 0;
    GLint resolution = -1, time = -1, cv = -1;
    double started = 0.0;
    double lastFrame = -1.0;
    bool applyPending = false;
    bool stopPending = false;
    bool accepted = false;
    std::string candidate;
    std::string activeSource;
    std::string error;

    explicit ShaderPreview(ShaderModule* owner) : module(owner) {}

    ~ShaderPreview() override {
        releaseProgram();
        if (buffer)
            glDeleteBuffers(1, &buffer);
    }

    void submit(std::string source) {
        candidate = std::move(source);
        error.clear();
        applyPending = true;
        stopPending = false;
        accepted = false;
    }

    void stop() {
        applyPending = false;
        stopPending = true;
        accepted = false;
        error.clear();
    }

    void releaseProgram() {
        if (program)
            glDeleteProgram(program);
        program = 0;
    }

    void onContextDestroy(const ContextDestroyEvent& e) override {
        releaseProgram();
        if (buffer)
            glDeleteBuffers(1, &buffer);
        buffer = 0;
        if (!applyPending && !stopPending && !activeSource.empty())
            submit(activeSource);
        OpenGlWidget::onContextDestroy(e);
    }

    void step() override {
        const double now = system::getTime();
        if (applyPending || stopPending || (program && now - lastFrame >= 1.0 / 60.0 - 0.001))
            setDirty();
        FramebufferWidget::step();
    }

    void drawFramebuffer() override {
        lastFrame = system::getTime();
        if (stopPending) {
            releaseProgram();
            activeSource.clear();
            stopPending = false;
        }
        if (applyPending) {
            applyPending = false;
            if (const GLuint replacement = linkShader(candidate, error)) {
                releaseProgram();
                program = replacement;
                resolution = glGetUniformLocation(program, "u_resolution");
                time = glGetUniformLocation(program, "u_time");
                cv = glGetUniformLocation(program, "u_cv");
                started = lastFrame;
                activeSource = candidate;
                accepted = true;
            }
        }

        ShaderDrawState restore;
        const auto size = getFramebufferSize();
        glViewport(0, 0, size.x, size.y);
        glClearColor(.015f, .02f, .03f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!program)
            return;
        if (!buffer) {
            const GLfloat vertices[] = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};
            glGenBuffers(1, &buffer);
            glBindBuffer(GL_ARRAY_BUFFER, buffer);
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        }
        glUseProgram(program);
        glUniform2f(resolution, size.x, size.y);
        glUniform1f(time, float(lastFrame - started));
        glUniform2f(cv, module->cv[0].load(std::memory_order_relaxed),
                    module->cv[1].load(std::memory_order_relaxed));
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
};

struct ShaderWidget : ModuleWidget {
    ShaderModule* const shader;
    CodeEditor* editor;
    ShaderPreview* preview = nullptr;
    CodeButton* layoutButton;
    unsigned revision;
    std::string message;

    explicit ShaderWidget(ShaderModule* module) : shader(module), revision(shader ? shader->revision : 0) {
        setModule(module);
        box.size = Vec(52 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT);
        for (float x : {RACK_GRID_WIDTH, box.size.x - 2 * RACK_GRID_WIDTH}) {
            addChild(createWidget<ScrewSilver>(Vec(x, 0)));
            addChild(createWidget<ScrewSilver>(Vec(x, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        }

        editor = new CodeEditor(++nextRunner, "GLSL");
        editor->box = Rect(Vec(12, 74), Vec(365, 257));
        editor->setText(shader ? shader->source : defaultShader);
        addChild(editor);
        if (shader) {
            preview = new ShaderPreview(shader);
            addChild(preview);
        }

        addButton("Apply", 12, 62, [this] { apply(); });
        addButton("Stop", 80, 56, [this] { if (preview) preview->stop(); });
        layoutButton = addButton("Preview only", 142, 95, [this] { toggleLayout(); });
        for (int i = 0; i < 2; ++i)
            addInput(createInputCentered<PJ301MPort>(Vec(745, 180 + i * 65), module, i));
        layout();
    }

    CodeButton* addButton(const char* label, float x, float width, std::function<void()> action) {
        auto* button = new CodeButton;
        button->text = label;
        button->box = Rect(Vec(x, 42), Vec(width, 24));
        button->action = std::move(action);
        addChild(button);
        return button;
    }

    void layout() {
        const bool previewOnly = shader && shader->previewOnly;
        editor->visible = !previewOnly;
        layoutButton->text = previewOnly ? "Show editor" : "Preview only";
        if (preview)
            preview->box = previewOnly ? Rect(Vec(12, 74), Vec(700, 282))
                                       : Rect(Vec(387, 74), Vec(325, 257));
    }

    void toggleLayout() {
        if (!shader)
            return;
        shader->previewOnly = !shader->previewOnly;
        layout();
    }

    void apply() {
        if (!preview)
            return;
        auto source = editor->getText();
        if (!source.empty() && source.back() == '\n')
            source.pop_back();
        if (source.size() > maxSourceSize || source.find('\0') != std::string::npos) {
            message = "Shader source must be at most 16 KiB and contain no NUL characters.";
            return;
        }
        message.clear();
        preview->submit(std::move(source));
    }

    void step() override {
        if (preview) {
            if (revision != shader->revision) {
                editor->setText(shader->source);
                preview->submit(shader->source);
                revision = shader->revision;
            }
            if (editor->visible == shader->previewOnly)
                layout();
            if (preview->accepted) {
                preview->accepted = false;
                if (preview->activeSource != shader->source) {
                    auto* action = new history::ModuleChange;
                    action->name = "edit GLSL shader";
                    action->moduleId = shader->id;
                    action->oldModuleJ = toJson();
                    shader->source = preview->activeSource;
                    action->newModuleJ = toJson();
                    APP->history->push(action);
                }
            }
        }
        ModuleWidget::step();
    }

    void draw(const DrawArgs& args) override {
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(args.vg, nvgRGB(28, 32, 36));
        nvgFill(args.vg);
        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgFillColor(args.vg, nvgRGB(222, 227, 231));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontSize(args.vg, 15);
        nvgText(args.vg, box.size.x / 2, 26, "GLSL Visualizer", nullptr);
        nvgFontSize(args.vg, 11);
        nvgText(args.vg, 745, 154, "CV 1", nullptr);
        nvgText(args.vg, 745, 219, "CV 2", nullptr);
        if (preview) {
            nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgText(args.vg, 250, 54, preview->applyPending ? "Compiling…"
                : preview->program ? "Running" : "Stopped", nullptr);
            const auto& error = message.empty() ? preview->error : message;
            if (!error.empty()) {
                nvgFillColor(args.vg, nvgRGB(248, 171, 146));
                nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
                nvgSave(args.vg);
                nvgIntersectScissor(args.vg, 12, 334, 700, 31);
                nvgTextBox(args.vg, 12, 336, 700, error.c_str(), nullptr);
                nvgRestore(args.vg);
            }
        }
        ModuleWidget::draw(args);
    }
};

} // namespace

Model* modelCardinalShader = createModel<ShaderModule, ShaderWidget>("Shader");
Model* modelCardinalCode = createModel<CodeModule, CodeWidget>("Code");
#endif
