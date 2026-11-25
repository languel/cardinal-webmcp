/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <app/Scene.hpp>
#include <context.hpp>
#include <engine/Engine.hpp>
#include <history.hpp>
#include <plugin.hpp>
#include "DistrhoUtils.hpp"
#include "plugincontext.hpp"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace rack;
using Json = std::unique_ptr<json_t, decltype(&json_decref)>;

// The browser calls this on the UI thread of the standalone WASM instance.
Context* webContext = nullptr;

struct ScopedContext {
    Context* const previous = contextGet();
    ScopedContext() { contextSet(webContext); }
    ~ScopedContext() { contextSet(previous); }
};

const char* requiredString(json_t* input, const char* key)
{
    const char* value = json_string_value(json_object_get(input, key));
    if (value == nullptr || *value == '\0')
        throw std::runtime_error(std::string("Expected a non-empty string for ") + key);
    return value;
}

int64_t identifier(json_t* input, const char* key)
{
    const std::string value = requiredString(input, key);
    if (value.size() > 19 || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error(std::string("Expected a decimal ID for ") + key);
    return std::stoll(value);
}

int index(json_t* input, const char* key, int min, int max)
{
    json_t* value = json_object_get(input, key);
    if (!json_is_integer(value) || json_integer_value(value) < min || json_integer_value(value) > max)
        throw std::runtime_error(std::string("Invalid integer for ") + key);
    return static_cast<int>(json_integer_value(value));
}

app::ModuleWidget* findModule(json_t* input, const char* key)
{
    auto* widget = APP->scene->rack->getModule(identifier(input, key));
    if (widget == nullptr)
        throw std::runtime_error(std::string("No live module for ") + key);
    return widget;
}

json_t* moduleSummary(app::ModuleWidget* widget)
{
    auto* model = widget->getModel();
    const auto grid = widget->getGridBox();
    return json_pack("{s:s,s:s,s:s,s:s,s:i,s:i,s:i}",
                     "id", std::to_string(widget->getModule()->id).c_str(),
                     "plugin", model->plugin->slug.c_str(), "model", model->slug.c_str(),
                     "name", model->name.c_str(), "x", static_cast<int>(grid.pos.x),
                     "row", static_cast<int>(grid.pos.y), "width", static_cast<int>(grid.size.x));
}

json_t* cableSummary(engine::Cable* cable)
{
    return json_pack("{s:s,s:s,s:i,s:s,s:i}",
                     "id", std::to_string(cable->id).c_str(),
                     "outputModule", std::to_string(cable->outputModule->id).c_str(),
                     "outputPort", cable->outputId,
                     "inputModule", std::to_string(cable->inputModule->id).c_str(),
                     "inputPort", cable->inputId);
}

json_t* patchSummary()
{
    json_t* modules = json_array();
    json_t* cables = json_array();
    auto* rack = APP->scene->rack;
    for (auto* widget : rack->getModules())
        json_array_append_new(modules, moduleSummary(widget));
    for (auto* widget : rack->getCompleteCables())
        json_array_append_new(cables, cableSummary(widget->getCable()));
    return json_pack("{s:o,s:o,s:b,s:b,s:b}", "modules", modules, "cables", cables,
                     "saved", APP->history->isSaved(), "canUndo", APP->history->canUndo(),
                     "canRedo", APP->history->canRedo());
}

std::string searchable(std::string value)
{
    for (char& c : value)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return value;
}

json_t* searchModules(json_t* input)
{
    std::istringstream query(searchable(requiredString(input, "query")));
    std::vector<std::string> terms;
    for (std::string term; query >> term;)
        terms.push_back(term);
    if (terms.empty())
        throw std::runtime_error("Enter a module name, plugin, or description");
    const int limit = json_object_get(input, "limit") ? index(input, "limit", 1, 50) : 20;
    json_t* results = json_array();
    for (auto* plugin : plugin::plugins)
    {
        for (auto* model : plugin->models)
        {
            if (model->hidden)
                continue;
            const auto label = searchable(plugin->slug + " " + model->slug + " " + model->name + " " + model->description);
            if (!std::all_of(terms.begin(), terms.end(), [&](const std::string& term) {
                    return label.find(term) != std::string::npos;
                }))
                continue;
            json_array_append_new(results, json_pack("{s:s,s:s,s:s,s:s}", "plugin", plugin->slug.c_str(),
                "model", model->slug.c_str(), "name", model->name.c_str(), "description", model->description.c_str()));
            if (json_array_size(results) == static_cast<size_t>(limit))
                return results;
        }
    }
    return results;
}

json_t* inspectModule(app::ModuleWidget* widget)
{
    json_t* info = moduleSummary(widget);
    json_t* params = json_array();
    json_t* inputs = json_array();
    json_t* outputs = json_array();
    for (auto* control : widget->getParams())
    {
        if (auto* param = control->getParamQuantity())
            json_array_append_new(params, json_pack("{s:i,s:s,s:f,s:f,s:f,s:s}",
                "id", control->paramId, "name", param->getLabel().c_str(),
                "value", param->getValue(), "min", param->getMinValue(), "max", param->getMaxValue(),
                "display", (param->getDisplayValueString() + param->getUnit()).c_str()));
    }
    for (auto* port : widget->getPorts())
    {
        auto* portInfo = port->getPortInfo();
        json_t* ports = port->type == engine::Port::INPUT ? inputs : outputs;
        json_array_append_new(ports, json_pack("{s:i,s:s}", "id", port->portId,
                                              "name", portInfo ? portInfo->getName().c_str() : ""));
    }
    json_object_set_new(info, "parameters", params);
    json_object_set_new(info, "inputs", inputs);
    json_object_set_new(info, "outputs", outputs);
    return info;
}

json_t* addModule(json_t* input)
{
    const char* pluginSlug = requiredString(input, "plugin");
    const char* modelSlug = requiredString(input, "model");
    auto* model = plugin::getModel(pluginSlug, modelSlug);
    if (model == nullptr || model->hidden)
        throw std::runtime_error("Model is unavailable. Search the installed module catalog.");
    const int x = index(input, "x", -4096, 4096);
    const int row = index(input, "row", -32, 32);
    auto* rack = APP->scene->rack;
    std::unique_ptr<engine::Module> instance(model->createModule());
    if (!instance)
        throw std::runtime_error("Could not instantiate the module");
    APP->engine->addModule(instance.get());
    auto* module = instance.release();
    const int64_t moduleId = module->id;

    std::unique_ptr<app::ModuleWidget> widget;
    try { widget.reset(model->createModuleWidget(module)); }
    catch (...)
    {
        if (APP->engine->getModule(moduleId) == module)
        {
            APP->engine->removeModule(module);
            delete module;
        }
        throw;
    }
    if (!widget)
    {
        APP->engine->removeModule(module);
        delete module;
        throw std::runtime_error("Could not create the module panel");
    }
    // Once created, the widget owns its module and removes it on destruction.
    widget->setGridPosition(math::Vec(x, row));
    if (!rack->requestModulePos(widget.get(), widget->getPosition()))
        throw std::runtime_error("This module would overlap an existing panel. Choose another position.");
    auto action = std::unique_ptr<history::ModuleAdd>(new history::ModuleAdd);
    action->setModule(widget.get());
    Json result(inspectModule(widget.get()), json_decref);
    rack->addModule(widget.get());
    widget.release();
    APP->history->push(action.release());
    return result.release();
}

json_t* connectCable(json_t* input)
{
    auto* rack = APP->scene->rack;
    auto* output = findModule(input, "outputModule")->getOutput(index(input, "outputPort", 0, std::numeric_limits<int>::max()));
    auto* dest = findModule(input, "inputModule")->getInput(index(input, "inputPort", 0, std::numeric_limits<int>::max()));
    if (!output || !dest)
        throw std::runtime_error("Invalid port. Inspect both modules for their port IDs.");
    if (auto* existing = rack->getTopCable(dest))
    {
        if (existing->outputPort == output)
            return cableSummary(existing->getCable());
        throw std::runtime_error("Input already has a cable. Disconnect it first.");
    }

    auto widget = std::unique_ptr<app::CableWidget>(new app::CableWidget);
    auto action = std::unique_ptr<history::CableAdd>(new history::CableAdd);
    widget->inputPort = dest;
    widget->outputPort = output;
    widget->color = rack->getNextCableColor();
    widget->updateCable();
    action->setCable(widget.get());
    Json result(cableSummary(widget->getCable()), json_decref);
    rack->addCable(widget.get());
    widget.release();
    APP->history->push(action.release());
    return result.release();
}

json_t* disconnectCable(json_t* input)
{
    auto* rack = APP->scene->rack;
    const int64_t id = identifier(input, "cable");
    auto* widget = rack->getCable(id);
    if (!widget)
        throw std::runtime_error("No live cable with that ID");
    auto action = std::unique_ptr<history::CableRemove>(new history::CableRemove);
    action->setCable(widget);
    rack->removeCable(widget);
    delete widget;
    APP->history->push(action.release());
    return json_pack("{s:s}", "disconnected", std::to_string(id).c_str());
}

json_t* setParameter(json_t* input)
{
    auto* widget = findModule(input, "module");
    const int id = index(input, "parameter", 0, std::numeric_limits<int>::max());
    auto* control = widget->getParam(id);
    auto* param = control ? control->getParamQuantity() : nullptr;
    if (!param)
        throw std::runtime_error("Invalid parameter. Inspect the module for its parameter IDs.");
    json_t* raw = json_object_get(input, "value");
    const double value = json_number_value(raw);
    if (!json_is_number(raw) || !std::isfinite(value) || value < param->getMinValue() || value > param->getMaxValue())
        throw std::runtime_error("Value is outside the parameter's raw range");
    auto action = std::unique_ptr<history::ParamChange>(new history::ParamChange);
    action->moduleId = widget->getModule()->id;
    action->paramId = id;
    action->oldValue = param->getValue();
    param->setImmediateValue(static_cast<float>(value));
    action->newValue = param->getValue();
    if (action->newValue != action->oldValue)
        APP->history->push(action.release());
    return json_pack("{s:i,s:f,s:s}", "parameter", id, "value", param->getValue(),
                     "display", (param->getDisplayValueString() + param->getUnit()).c_str());
}

json_t* execute(const std::string& operation, json_t* input)
{
    if (operation == "host_parameters" || operation == "host_parameter")
    {
        auto* context = static_cast<CardinalPluginContext*>(APP);
        if (operation == "host_parameters")
            return json_pack("{s:i}", "count", static_cast<int>(context->parameterCount));

        const int slot = index(input, "slot", 1, context->parameterCount) - 1;
        const auto* number = json_object_get(input, "value");
        const double value = json_number_value(number);
        if (!json_is_number(number) || !std::isfinite(value) || value < 0.0 || value > 10.0)
            throw std::runtime_error("Expected a host parameter value between 0 and 10");
        context->parameters[slot] = static_cast<float>(value);
        return json_pack("{s:f}", "value", value);
    }
    if (operation == "patch") return patchSummary();
    if (operation == "search") return searchModules(input);
    if (operation == "inspect") return inspectModule(findModule(input, "module"));
    if (operation == "add") return addModule(input);
    if (operation == "remove")
    {
        auto* widget = findModule(input, "module");
        auto id = std::to_string(widget->getModule()->id);
        widget->removeAction();
        return json_pack("{s:s}", "removed", id.c_str());
    }
    if (operation == "connect") return connectCable(input);
    if (operation == "disconnect") return disconnectCable(input);
    if (operation == "set") return setParameter(input);
    if (operation == "undo")
    {
        if (!APP->history->canUndo())
            throw std::runtime_error("There is no rack edit to undo");
        APP->history->undo();
        return patchSummary();
    }
    throw std::runtime_error("Unknown Cardinal operation");
}

} // namespace

void cardinalSetWebMCPContext(rack::Context* context)
{
    webContext = context;
}

extern "C" EMSCRIPTEN_KEEPALIVE const char* cardinalWebMCP(const char* operation, const char* args)
{
    static std::string response;
    Json result(nullptr, json_decref);
    try
    {
        if (webContext == nullptr)
            throw std::runtime_error("The Cardinal rack is still loading");
        Json input(json_loads(args, JSON_REJECT_DUPLICATES, nullptr), json_decref);
        if (!json_is_object(input.get()))
            throw std::runtime_error("Expected a JSON object");
        const ScopedContext scope;
        result.reset(execute(operation, input.get()));
    }
    catch (const std::exception& error)
    {
        result.reset(json_pack("{s:s}", "error", error.what()));
    }
    catch (...)
    {
        result.reset(json_pack("{s:s}", "error", "Cardinal could not complete the operation"));
    }
    std::unique_ptr<char, decltype(&std::free)> data(json_dumps(result.get(), JSON_COMPACT), std::free);
    response = data ? data.get() : "{\"error\":\"Could not serialize the result\"}";
    return response.c_str();
}
