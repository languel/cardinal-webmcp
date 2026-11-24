# Cardinal WebMCP classroom deployment

[Open the classroom site](https://languel.github.io/cardinal-webmcp/).
This is Cardinal 26.02 with the WebMCP bridge described in
[WEBMCP.md](../docs/WEBMCP.md). The starter patch uses Cardinal and Fundamental modules.

The Classroom Pages workflow verifies the release archive against
`cardinal-wasm.sha256`, adds the starter and guide, and publishes the result to
GitHub Pages. The archive contains the CardinalNative HTML, JavaScript, WASM,
resource data, and the WebMCP registration module. The release is tagged on the
source revision used for the deployment. The local release used the pinned
submodules, PawPaw's Emscripten 3.1.27 SDK, and the Surge WASM compatibility
adjustments described in the build notes and upstream WASM workflow.

The original Cardinal source and dependency references are included in this repository.
See the [license](../LICENSE) and the [upstream project](https://github.com/DISTRHO/Cardinal).
