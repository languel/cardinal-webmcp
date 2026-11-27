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
Use an optimized WASM link and ensure the generated libarchive configuration
defines both `HAVE_LIBZSTD` and `HAVE_ZSTD_H` so compressed patches can open.

The original Cardinal source and dependency references are included in this repository.
See the [license](../LICENSE) and the [upstream project](https://github.com/DISTRHO/Cardinal).

## Live coding and performance

Build the browser version of CardinalNative from this source to use the
[livecode collection by languel](../docs/CARDINAL-MODULES.md#livecode).
Search for **livecode** in the module browser to find Code, GLSL, p5.js,
Three.js, Play.core, Strudel, Browser Transport, and Keys. Patches saved with
these modules under **Cardinal** can still open.

- Open the [Code CV demo](../patches/code-cv-demo.vcv) to try inline JavaScript and rack CV.
  Patch files store the source and layout. Reopened patches start stopped.
- Open the [Browser Transport demo](../patches/browser-transport-demo.vcv) to try two
  linked Strudel patterns. Start **Run** on the transport, then click ▶ on a
  Strudel panel. Set quantization to **BAR** to queue edits with **Ctrl+Enter**
  and stops with **Ctrl+.** while the current pattern plays through to the downbeat.
- Add [Keys](../docs/CARDINAL-MODULES.md#keys) to play a rack voice with the mouse or computer
  keyboard. Click the piano to focus it. Patch **Pitch**, **Gate**, and **Vel**
  to **Host MIDI** and use **Engine → Enable/Reconnect MIDI** for external MIDI.

Open **Edit → Bindings** to learn a keyboard key or MIDI note or CC and assign it to
a Host Parameter slot. Use **Host Params Map** to connect that slot to a module
control, such as a sequencer's Run button. Bindings are stored in this browser.
The View sliders and the Host Params Map slot selector follow the pointer position.

The public site uses the WebAssembly archive pinned by the Pages workflow. A
source push alone does not replace that archive or publish the site.

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
