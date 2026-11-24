# Cardinal WebMCP classroom deployment

[Open the classroom site](https://languel.github.io/cardinal-webmcp/).
This is Cardinal 26.02 with the WebMCP bridge described in
[WEBMCP.md](../docs/WEBMCP.md). The starter patch uses Cardinal and Fundamental modules.

The Classroom Pages workflow verifies the release archive against
`cardinal-wasm.sha256`, adds the starter and guide, and publishes the rack at the site root. The
classroom guide and Médiathèque are linked from the rack’s Help menu. The archive contains the CardinalNative JavaScript, WASM, resource data,
and the WebMCP registration module. Pages assembles the HTML from source. The release is tagged on the
source revision used for the deployment. The local release used the pinned
submodules, PawPaw's Emscripten 3.1.27 SDK, and the Surge WASM compatibility
adjustments described in the build notes and upstream WASM workflow.

The original Cardinal source and dependency references are included in this repository.
See the [license](../LICENSE) and the [upstream project](https://github.com/DISTRHO/Cardinal).

## Open a patch from a URL

Add a query parameter to the classroom site URL.

- `?patchurl=rack%2Ffm-filter-sequence.vcv` opens a patch published with the app.
- `?patchstorage=wasm-house-loop` opens a Cardinal patch from PatchStorage.
- `?patchurl=<URL-encoded .vcv URL>` opens a public file, including one in
  Médiathèque. Public GitHub file pages are converted to raw file URLs.
  Other file hosts must allow cross-origin requests from the classroom site.

The loader runs in the browser and accepts JSON and Zstandard-compressed
Cardinal patches. Patch bytes stay out of the address bar. The older
`/rack/` and FM starter links redirect to the root app.
