# Cardinal WebMCP

The CardinalNative web build registers tools through the browser's
`document.modelContext` API. Open Cardinal in a WebMCP-capable browser on
localhost or HTTPS, then let the browser agent discover tools on that tab.
Each tool reads or changes the rack in that tab. Rack edits share the editor's
Undo history and can be saved from Cardinal's File menu.

Build the web version as described in [BUILDING.md](BUILDING.md), then serve the
build files from one local origin. The tool module `CardinalWebMCP.mjs` must be
alongside `CardinalNative.html` and its JavaScript, WASM, and data files. Serve
`CardinalBindings.mjs` and `CardinalCode.mjs` from the same directory for the
bindings catalog and the Code module runner.

```sh
python3 -m http.server 8786 --bind 127.0.0.1 --directory bin
```

Open `http://127.0.0.1:8786/CardinalNative.html`. Browser audio may need a click
on the rack before playback starts.

On macOS with the pinned PawPaw/Emscripten toolchain, the Surge dependency
may configure with `CMAKE_SYSTEM_NAME=Generic`. If Surge reports that
`_stricmp` is undefined, update its `src/common/CMakeLists.txt` so the branch
that defines `LINUX=1` is `elseif(UNIX OR CMAKE_SYSTEM_NAME STREQUAL "Generic")`.
The repository's WASM workflow also shows the Surge SIMD compatibility patch.

| Tool | Purpose |
| --- | --- |
| `cardinal_patch` | Read the live modules, layout, cable endpoints, and undo state |
| `cardinal_search_modules` | Find installed modules by plugin, slug, or name |
| `cardinal_inspect_module` | Read ports, parameter ranges, and current values |
| `cardinal_add_module` | Place an installed module at an empty grid position |
| `cardinal_remove_module` | Remove a module and its connected cables |
| `cardinal_connect` | Connect two ports |
| `cardinal_disconnect` | Remove one cable |
| `cardinal_set_parameter` | Set a parameter by its ID and raw value |
| `cardinal_undo` | Undo the latest edit, including edits made in the UI |

For a first edit, call `cardinal_patch` and search for `Fundamental`. Add a
module using the returned plugin and model strings and an empty `x` and `row`
position. `x` is measured in HP. Use the port and parameter IDs from the live
module to connect or adjust it. Module and cable IDs are decimal strings to
preserve their full precision. Port and parameter IDs are integers.

To try a read call in the browser console:

```js
const context = document.modelContext;
const tools = await context.getTools();
const patch = tools.find(tool => tool.name === 'cardinal_patch');
await context.executeTool(patch, {});
```
