/* SPDX-License-Identifier: GPL-3.0-or-later */

const moduleId = { type: 'string', pattern: '^[0-9]+$', description: 'Module ID from cardinal_patch or cardinal_add_module.' };
const portId = { type: 'integer', minimum: 0 };
const tool = (name, description, operation, properties, required = Object.keys(properties), readOnlyHint = false) => ({
  name: `cardinal_${name}`,
  description,
  operation,
  inputSchema: { type: 'object', properties, required, additionalProperties: false },
  annotations: { readOnlyHint },
});

const tools = [
  tool('patch', 'Read the current rack layout, module IDs, cable endpoints, and undo state.', 'patch', {}, [], true),
  tool('search_modules', 'Find installed modules by name, slug, or description. Search “Fundamental filter” for stock filters.', 'search', {
    query: { type: 'string', minLength: 1 },
    limit: { type: 'integer', minimum: 1, maximum: 50 },
  }, ['query'], true),
  tool('inspect_module', 'Read a live module’s input and output port IDs, parameter IDs, raw ranges, and current settings.', 'inspect', {
    module: moduleId,
  }, ['module'], true),
  tool('add_module', 'Add an installed module at an empty rack position. x is measured in HP and row in rack rows, as returned by cardinal_patch.', 'add', {
    plugin: { type: 'string' },
    model: { type: 'string' },
    x: { type: 'integer', minimum: -4096, maximum: 4096 },
    row: { type: 'integer', minimum: -32, maximum: 32 },
  }),
  tool('remove_module', 'Remove a live module and its cables. The edit can be undone.', 'remove', {
    module: moduleId,
  }),
  tool('connect', 'Connect a module output to an empty input. An existing identical connection is returned as-is.', 'connect', {
    outputModule: moduleId,
    outputPort: portId,
    inputModule: moduleId,
    inputPort: portId,
  }),
  tool('disconnect', 'Remove one cable by ID from the live patch. The edit can be undone.', 'disconnect', {
    cable: { type: 'string', pattern: '^[0-9]+$' },
  }),
  tool('set_parameter', 'Set a live knob or switch to a raw value within its range. Inspect the module for its parameter IDs and ranges.', 'set', {
    module: moduleId,
    parameter: { type: 'integer', minimum: 0 },
    value: { type: 'number' },
  }),
  tool('undo', 'Undo the most recent rack edit, whether made by a person or an agent.', 'undo', {}),
];

export async function registerCardinalTools(modelContext, wasmModule) {
  const invoke = wasmModule.cwrap('cardinalWebMCP', 'string', ['string', 'string']);
  const registrations = new AbortController();
  try {
    for (const { operation, ...definition } of tools) {
      await modelContext.registerTool({
        ...definition,
        execute(input = {}, { signal } = {}) {
          signal?.throwIfAborted();
          try {
            return JSON.parse(invoke(operation, JSON.stringify(input)));
          } catch {
            return { error: 'The Cardinal rack could not complete this tool call.' };
          }
        },
      }, { signal: registrations.signal });
    }
  } catch (error) {
    registrations.abort();
    throw error;
  }
}
