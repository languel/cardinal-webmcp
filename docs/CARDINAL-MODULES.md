# Cardinal Modules

This file contains documentation for the DISTRHO Cardinal modules.

## Main modules

### AIDA-X

![screenshot](Module_AIDA-X.png)

[AIDA-X](https://github.com/AidaDSP/AIDA-X) is an Amp Model Player leveraging AI and machine learning, with a target of providing high fidelity simulations of guitar amplifiers.  
It is also possible to run entire signal chains consisting of any combination of amp, cab, dist, drive, fuzz, boost and eq.

The module loads AIDA-X files that have been trained to match a desired sound output.  
Right-click on the module and select "Load model file..." to load an AIDA-X model file from disk.

A quick model pack can be downloaded from [AIDA DSP's Google Drive folder](https://drive.google.com/drive/folders/18MwNhuo9fjK8hlne6SAdhpGtL4bWsVz-).

Check out the [MOD Forum's Neural Modelling section](https://forum.mod.audio/c/neural/62) for an online place for discussion, sharing and all things related to Amp Models.

This module is a port of the [AIDA-X audio plugin](https://github.com/AidaDSP/AIDA-X), also available separately.

### Audio File

![screenshot](Module_AudioFile.png)

This is a simple audio file player plugin.  
You can load files by using right-click menu options.

Transport is rolling as long as the plugin is enabled and a file has been loaded.  
You can also sync to host transport, which will play in frame-perfect sync, useful if you need a few loops playing together.

Files are streamed from disk if longer than 30 seconds in length, otherwise loaded entirely on RAM.

Even though this player plugin can loop files, it is not an audio looper.  
Audio can loop back to the beginning if enabled, but it does not resample or pitch-shift to fit the host BPM.  
The audio files are played back as-is, with resampling only done when needed to match the host sample rate.

### Audio To CV Pitch

![screenshot](Module_AudioToCVPitch.png)

The Audio To CV Pitch module is a tool that turns your audio signal into CV pitch and CV gate signals.  
This allows audio from instruments (such as guitars) to play and control synth sounds and effects.

It detects the pitch in your incoming audio signal and outputs a 1V/Oct CV pitch signal on the "Pitch Out" CV port.  
The "Gate" CV port sends out 10V while a pitch is detected, and resets to 0V when the pitch can no longer be detected.

There is an Octave right-click option that allows you to shift the detected pitch up or down by a maximum of 4 octaves.  
When set to 0, it will output the same pitch as is detected on the input.

Then the "Hold Output Pitch" right-click option sets whether the plugin resets its outputs to 0, or holds the last detected pitch.

The Sensitivity parameter can be increased to detect quieter signals, or decreased to reduce artifacts.  
The Confidence Threshold can be increased to make sure the correct pitch is being output, or decrease it to get a faster response time.  
And finally, the Tolerance parameter influences how quickly you can change pitch, turn it down for a more accurate pitch output, or turn it up to make it easier to jump from one pitch to the next.

### Carla

![screenshot](Module_Carla.png)

[Carla](https://kx.studio/Applications:Carla) is a fully-featured modular audio plugin host.  
This is a Cardinal/Rack module of Carla with 2 audio and 8 CV IO.  
Double-click on the panel to show the Carla's own window.

Note that it is required for Carla to be installed on your system in order to show its GUI.  
Cardinal Windows installers ship with their own Carla version, for convenience.  
On Linux and macOS you will need Carla to be installed in their typical locations.

MIDI input and output are possible with the use of expander modules.

By default Carla will have an offset on its CV inputs and outputs so that 0 to 10v from Carla generates -5 to 5v in Cardinal, and vice-versa.  
This allows to easily route between bipolar and unipolar+ signals.  
You can change this with right-click menu options, in case you want no offset to be applied.

### Ildaeil

![screenshot](Module_Ildaeil.png)

[Ildaeil](https://github.com/DISTRHO/Ildaeil) is a mini-plugin host based on [Carla](https://kx.studio/Applications:Carla).  
This is a Cardinal/Rack module of Ildaeil with 2 audio IO.

Currently Ildaeil supports (Carla) internal, LV2 and JSFX plugins.  
More plugin support will be added in future releases.

There is no way to automate hosted plugin parameters yet.

MIDI input and output are possible with the use of expander modules.

### Host Audio

![screenshot](Module_HostAudio.png)

A module for getting audio to and from your Host/DAW.  
Works just like the Rack equivalent, including a DC filter that is enabled by default on the stereo version, disabled on the 8 IO variant.

On the stereo version, connecting to the Left/Mono input jack will automatically mirror that signal on the Right, unconnected jack.  
This is the same behaviour as present in Rack, but without signal delay.

Note that, unlike Rack, Cardinal does not provide a 16 IO audio module.

### Host CV

![screenshot](Module_HostCV.png)

For getting CV to and from your Host/DAW.

Just as with the Carla module, you can enable right-click menu options to automatically apply an offset to the CV signal.  
For convenience this mode is split on the 1-5 and 6-10 ports, in case you need some to be bipolar and some unipolar.

Note that this module does nothing on Cardinal's FX and Synth variants, as they do not have host-exposed CV IO.

### Host MIDI

![screenshot](Module_HostMIDI.png)

For getting regular MIDI to and from your Host/DAW.  
It has pretty much the same options and functionality as the Rack MIDI2CV and CV2MIDI modules, but combines them into one.

Clock pulses are not available on this module in Cardinal, prefer to use Host Time module for that.

### Host MIDI CC

![screenshot](Module_HostMIDICC.png)

For getting regular MIDI CCs to and from your Host/DAW.  
It has pretty much the same options and functionality as the Rack MIDI-CC2CV and CV2MIDI-CC modules, but combines them into one.

Direct channel pressure and pitchbend ports are available for convenience.

### Host MIDI Gate

![screenshot](Module_HostMIDIGate.png)

For getting regular MIDI Notes as Gates to and from your Host/DAW.  
It has pretty much the same options and functionality as the Rack MIDI-Gate2CV and CV2MIDI-Gate modules, but combines them into one.

### Host MIDI CC Map

![screenshot](Module_HostMIDIMap.png)

For mapping MIDI CCs from your Host/DAW into module parameters.  
It has pretty much the same options and functionality as the Rack MIDI-Map.

### Host Parameters

![screenshot](Module_HostParameters.png)

For automating CV sources from the host side, this module directly converts the 24 Cardinal host-exposed parameters into CV signals inside Cardinal.

The parameters have an internal slew limiter in order to smooth out the changes over time.  
The fall time of the smoothing filter is based on the current host block size and sample rate.  
You can disable this smoothing filter using right-click menu options.

Clicking on an input jack from this module will send a "touch"/"edit" event to the plugin host / DAW,  
required for allowing to create parameter automation lines in some hosts.

### Host Parameters Map

![screenshot](Module_HostParametersMap.png)

For automating modules loaded inside Cardinal from within host side, using the 24 Cardinal host-exposed parameters.

This is similar to Host MIDI CC Map, but instead of having MIDI CC from the host as source of events, it uses host-exposed parameters.

Simply click on the last item on the list of mappings (which will say "click here to map" or "unmapped" depending if you have already any mappings or not),
and then move a Cardinal-side module knob or controllable widget.

Anytime you click to either do a new mapping or edit an existing one, a few options appear for setting host parameter, smooth on/off and invert on/off.  
It is not possible to reassign an existing mapping to a different knob.  
Right-clicking an item on the mapping list will remove it.

Changing module knobs from Cardinal side will not send a parameter change event to the host.  
This module works more in a "one to many" control scheme, where 1 single host parameter can control many cardinal modules, but never the other way around.  
If you need to send a "parameter change" signal from Cardinal, use the "Host Parameters" module.

### livecode

The **livecode** collection by **languel** includes Browser Transport, Keys,
Code, Strudel, and the GLSL, p5.js, Three.js, and Play.core editors. Older
patches using the original Cardinal module names load into the collection.

#### Keys

Click the on-panel piano to play with the mouse and focus it for computer
keyboard input. The labels on the keys show two octaves of shortcuts.
**Octave** shifts both rows, and **Velocity** sets the velocity for new notes.
Click elsewhere to release the keyboard.

Patch **Pitch**, **Gate**, and **Vel** to a polyphonic voice. To send MIDI
to a browser MIDI device, patch the same outputs to the corresponding
inputs on **Host MIDI**, then choose **Engine → Enable/Reconnect MIDI**.

### Browser Transport

In the web standalone, open **Engine → Browser Transport** to enable a
shared clock, set its tempo, and select **BEAT**, **BAR**, or **OFF** for
linked Strudel launches, edits, and stops. **Shift+Space** starts or
stops it with the rack focused. Stopping returns the clock to bar 1 and
keeps linked patterns armed. These controls apply to the current browser
session.

Add the livecode **Browser Transport** module to control the same clock from
the rack. The large display shows bar and beat above elapsed time. **RUN**
starts or stops the clock, **SYNC** returns to bar 1, and **TAP** estimates
tempo from taps one beat apart. The matching input jacks accept trigger
pulses. **TAP** can follow the tempo of a patched beat clock, and **SYNC**
aligns the downbeat. Set **METER** and **QUANTIZE** on the panel. BPM counts
the selected beat unit, and changing meter returns the clock to bar 1.
The output jacks mirror Host Time, with BPM available while stopped too.

**Host Time** reads this clock when it is enabled. Existing patches wired to
Host Time continue to use its **Playing**, **Reset**, **Bar**, **Beat**, and
**Step** outputs. Patch **Bar** to the EnigmaCurry **Transport** module's
**CLK** if you want its counter and quantized recording to count bars.

Strudel follows the browser clock when its **Follow Browser Transport**
module option is on. Click ▶ to arm while the transport is stopped or to
queue a launch while it plays. Each pattern begins at the start of its own
cycle on the shared grid. One linked Strudel cycle spans one bar of the
current meter. While playing, **Ctrl+Enter** (or **Cmd+Enter**)
prepares an edit and switches patterns at the next available boundary,
keeping the current pattern playing in the meantime. **Ctrl+.** or clicking
▶ again queues a stop at that boundary. An unlinked module
can use its own `setcpm()` or **CLK** input. The linked mode takes its tempo
from Browser Transport.

[Try the browser transport demo](../patches/browser-transport-demo.vcv).

### Host Time

![screenshot](Module_HostTime.png)

For getting time information from your DAW as precise clock triggers.

Unlike Rack, Cardinal does not use PPQN (pulses per quarter note) values, but BBT (bar-beat-tick) instead.

The top of the panel has a timer display to help understand how and what the host is doing.  
The first line is the song time in seconds, and second line is the BBT.

The meaning of the outputs is:

- Playing: On if the host transport is rolling or Off if paused/stopped
- Reset: Trigger for when the host transport has been relocated or has been restarted
- Bar: Trigger for bar change
- Beat: Trigger for beat change
- Step: Trigger for step change
- Bar Phase: 0-10v smooth ramp for the bar position (with bar division being a right-click option)
- Beat Phase: 0-10v smooth ramp for the beat position
- BPM: Beats per minute in [Rack standard](https://vcvrack.com/manual/VoltageStandards#Pitch-and-Frequencies), supported by Impromptu Clocked/Clkd and ZZC Clock

## Extra modules

### Blank

![screenshot](Module_Blank.png)

Every collection needs a blank panel.  
Cardinal features Hatsune Miku on its blank panel, because yes. :)

Actually, it is just that CC-NC artwork and resources are very common-place within the Rack plugins ecosystem.  
Typically we would want to avoid those, but when you got pretty much every author using CC-NC, let's go all in then...  
So yes, the design used on this panel is [CC-NC licensed](https://piapro.net/intl/en_for_creators.html).

If you ask why Miku, well, why not. And why not even something else.
If you find other content we can use, under clear license terms, maybe it could be added as blank panel too.

Sometimes we just need to have fun. :)

### Sassy Scope

![screenshot](Module_SassyScope.png)

The virtual oscilloscope from the [Sassy Spreadsheet](https://github.com/jarikomppa/sassy) project.  
Features time and frequency analysis, input-based trigger sync and allows to pause the analysis for careful inspection.

Line colors follow the color of the cable connected to its respective input.

### Expanders MIDI

![screenshot](Module_ExpandersMIDI.png)

These are expanders for Carla and Ildaeil, allowing MIDI input and output.  
When connected (that is, standing next to them) the graphics on Carla/Ildaeil side will change a bit to indicate it is being used.

### MPV

This is a super experimental embed video player inside Cardinal.

It is mostly just a test for embedding external windows inside Cardinal, `mpv` simply provides a nice way to do it.

This is not intended to be a serious module in any way, purely experimental and made for development purposes.

### glBars

![screenshot](Module_glBars.png)

An OpenGL bars visualization, as seen in XMMS and XBMC/Kodi.

It is purely cosmetic.

This module was done early in Cardinal's development as a way to test drawing a module using OpenGL.  
It remains as a test case and example for others to base new modules on.

### Editor panel conventions

Use a neutral dark gray face and let the editor or preview fill the center.
Keep controls in a shallow strip and use the module browser for names.
Stack CV jacks vertically, with `IN` on the left and `OUT` on the right.
Input jacks sit directly on the panel and output jacks sit on a metal plate.
A warm red LED shows active processing and a dim gray lens shows the idle state.
Editors and previews stay readable as room brightness goes down. Active LEDs
follow Rack’s Light bloom setting.

### Code

Code runs a JavaScript function in Cardinal Web. Patch CV, edit the JavaScript
on the rack panel, then click **Run**. Click the lit button to stop. With focus
in the editor or the pointer over the module, **⌘↵** runs the current source
and **⌘.** stops. **Ctrl+Enter** and **Ctrl+.** also work.

Right-click the panel and choose **Inputs** or **Outputs** to set the jack
count from zero to six. Each defaults to two. Unplug a cable before hiding its
jack. The patch saves both counts.

```js
function process({ input, time }) {
  return [input[0] * 0.5 + input[1], 5 * Math.sin(2 * Math.PI * time)];
}
```

`input` contains the configured input voltages in jack order, and `time` is
seconds since Run. Return voltages in output jack order. Missing values output
0 V and extra values are ignored. The inputs and outputs are monophonic. The
browser evaluates the function up to 30 times a second and holds the last
output between calls. Outputs are limited to ±10 V. Use this for modulation
and control signals. Use rack modules for audio-rate processing or precise
clock and trigger timing.

Run stores the source with the module after the first successful evaluation.
**Collapse** makes the panel compact while the code continues running. **Show
editor** expands it when there is room to its right. Stopping sets the outputs to 0 V. Reopened patches start stopped and need a click on Run.

[Try the CV demo patch](../patches/code-cv-demo.vcv) with 8vert, two Code
modules, and Scope. Run both editors. The first halves the 6 V from 8vert, and
the second adds 1 V. Scope shows 4 V on channel 1 and the first module’s sine
LFO on channel 2.

### GLSL Visualizer

Edit a GLSL ES 1.00 or 3.00 fragment shader and click **▶** to render it in the rack. Click the lit button to stop.
The same shortcuts apply. Right-click and choose **Inputs** to set the number
of CV jacks from zero to six. The two output jacks carry left and right shader audio.
Start with the [GLSL visualizer demo](../patches/glsl-visualizer-demo.vcv).
The first 8vert knob changes the ring's radius, and the second changes its phase
and color. These are the uniforms available to a shader:

| Uniform | GLSL type | Value |
| --- | --- | --- |
| `u_resolution` | `vec2` | Preview width and height in pixels |
| `u_time` | `float` | Seconds since the shader was applied |
| `u_cv` | `vec2`, `vec3`, or `vec4` | First two, three, or four input voltages |
| `u_inputs` | `float[6]` | All six input positions in jack order. Hidden or unpatched inputs supply 0 V. |

**▶** saves the last successfully compiled source with the patch. A compile
error leaves the working shader playing. The view selector offers **Side by side**,
**Preview only**, and **Overlay**. Overlay puts the transparent editor directly
over the image. The view is saved with the patch.

Paste Shadertoy `mainImage(out vec4 color, in vec2 pixel)` directly into the
editor. The module supplies `iResolution`, `iTime`, `iTimeDelta`, `iFrame`,
`iMouse`, `iDate`, and `iSampleRate`. Shadertoy texture channels with no source
sample black. Shadertoy project imports and Buffer A–D graphs are outside the
single editor's shader format.

Twigl fragments can use `r`, `m`, `t`, `f`, `o`, and `FC`, with or without
`void main()`. `FC.xyz`, `fragCoord.xyz`, and `fragcoord.xyz` expose the
same coordinates as `gl_FragCoord.xyz`. `b` reads the previous frame.
The two-target Twigl form uses `o0/o1` and `b0/b1`. The first target is shown.

For GPU audio, define `vec2 mainSound(int sampleIndex, float seconds)` or
`vec2 mainSound(float seconds)`. The shader can also define `mainImage` in
the same editor. Return stereo samples in ±1 and patch the two audio jacks
to a mixer or Host Audio. Stopping clears the preview and silences the jacks.
The audio render needs the browser's graphics loop to keep running.

The [shader sound demo](../patches/glsl-sound-demo.vcv) starts in Overlay and
routes a quiet stereo tone to Host Audio and Scope. Press **▶** to start.

Right-click inside a code editor to choose a theme and font, toggle line
numbers and whitespace, or adjust code opacity. **Text background** adds an
optional adjustable color only behind non-whitespace glyphs. The available
fonts are Share Tech Mono, Roboto Mono, Fira Mono Bold, and Proggy Clean.
Appearance settings apply to the current editor instance. The horizontal
scrollbar appears only when the text exceeds the available width.

Use **Alt+Z** to toggle wrap, **Cmd/Ctrl+Z** to undo, and **Cmd/Ctrl+Shift+Z**
to redo. **Alt+Shift+F** formats the editor contents without running them.
Formatting uses Prettier and its GLSL plugin, loaded on demand, and can be
undone as one edit. Each editor starts with a transparent backdrop and the
Desaturated theme. Use **Cmd+Shift++** or
**Cmd+Shift+−** with the editor focused to change its font size. Ctrl works
as an alternative to Cmd.

Use **Cmd+Shift+,** and **Cmd+Shift+.** to adjust room brightness in
10-percentage-point steps. Ctrl also works for these shortcuts.
Use **Ctrl+.** to stop when the browser intercepts **Cmd+.**.

Press **F** over the rack for **Frame All**, or hover a module and press **.**
to frame it. With the pointer over empty rack space, **.** frames the selected
modules. These commands are also in **View**. Click outside the code editor
before using the rack shortcuts. Hover a module and press **Shift+F** for
single-module fullscreen. Press **Shift+F** again or **Escape** to restore
the previous rack view.

### Text Editor

![screenshot](Module_TextEditor.png)

A text editor inside Cardinal, with support for syntax highlighting.

It supports copy and paste, undo and redo, and **Cmd+/** or **Ctrl+/** to
toggle comments on the selected lines. With no selection, the shortcut
toggles the current line. The live code editors share the same shortcut.

The panel is resizable using the left and right borders.  
Moving the panel around can be done by grabbing the top status bar of the editor.

### p5.js, Three.js, and Play.core ASCII

These visual modules share the GLSL editor, **Side by side**, **Preview only**,
and **Overlay**. Use **▶** or **Cmd/Ctrl+Enter** to apply a sketch and
**Ctrl+.** to stop. Right-click the panel to set **Inputs** and **Outputs**
from zero to six. The output jacks hold the latest voltage from the sketch
and return to zero when stopped. They carry frame-rate CV.

- **p5.js Visualizer** accepts global `setup()/draw()` sketches and
  `function sketch(p)` for instance mode. Cardinal creates and drives the
  p5 instance.
- **Three.js Visualizer** provides `THREE` and a WebGL `renderer`. Define
  `draw()` and call `renderer.render(scene, camera)` each frame. ES module
  imports from `three` and `three/addons/` use the module’s pinned runtime version.
- **Play.core ASCII** accepts `export const settings` and
  `export function main(coord, context)`. Return a character or an object
  with `char`, `color`, and optional `backgroundColor`. It supports the
  Play.core lifecycle hooks and utility imports from `/src/modules/`.

All three sketches can read `rack.input` (volts), `rack.time` (seconds),
`rack.width`, `rack.height`, and `rack.pointer` (normalized coordinates).
Assign `rack.output[0] = 2.5` to send 2.5 V from the first output. The ASCII
callback also receives Play.core's `context.time` in milliseconds. The
p5.js and Three.js runtimes and optional Play.core utility imports load
from pinned jsDelivr URLs. Use self-contained sketches. Running code in
an iframe isolates it from the rack's DOM and storage, but a blocking loop
in a sketch can still stall the browser.

[Try the three visual modules](../patches/visual-code-demo.vcv).

### Strudel

[Open the Strudel demo](../patches/strudel-demo.vcv) or add a **Strudel** module
from the browser's module picker. Patch the top two **OUT** jacks to Host Audio
(left above right). Click ▶ to start or stop. **Ctrl/Cmd+Enter** applies
the editor contents and **Ctrl/Cmd+.** stops playback. Applied code is stored
with the patch. Playback begins when you run the code.

The editor supports Strudel mini-notation, named and `$:` pattern lines,
and tempo commands such as `setcpm(120 / 4)`. The browser loads the Strudel
runtime, built-in synths, and optional drum samples on first use.

[Try the Strudel clock and audio I/O demo](../patches/strudel-io-demo.vcv).
Connect a gate or trigger clock to **CLK**. Strudel follows its pulse interval
after two rising edges. Set **Clock pulses per beat** in the module menu
for clocks faster than one pulse per quarter note. The clock sets tempo with
four beats per cycle. It holds the last tempo when pulses stop and returns to
the pattern tempo when you unplug the cable. This is tempo following.

Patch audio to **L IN** and **R IN**, then use `s("rack")` as a live sound
source. With only **L IN** patched the signal feeds both channels. Incoming
audio passes through the browser audio buffer and can be gated and processed
with Strudel patterns. The top two outputs carry the default stereo mix.
Use a separate orbit with `.orbit(2).channels("3:4")` for the lower stereo pair.

Choose **Enable MIDI output** in the module menu and grant Web MIDI
permission in a supported browser. Use `note("c4 e4").midichan(2).midi()`
for the first MIDI output, or pass a device name to `.midi('Device Name')`.
The output sends note-on and note-off messages.

[Try the Strudel visuals demo](../patches/strudel-visuals-demo.vcv).
Use `pianoroll()`, `punchcard()`, `spiral()`, `pitchwheel()`, `scope()`,
`spectrum()`, `fscope()`, or `wordfall()` on a pattern. Cardinal also accepts
the REPL's underscore forms such as `._punchcard()` and stacks those visuals
in the module's preview. Choose **Side by side**, **Overlay**, or **Preview only**
from the layout menu.

Playing mini-notation tokens light up in the editor. Use `.color("#81b5cb")`
or `.markcss('color: cyan; text-decoration: underline')` to style them.
Cardinal renders text and background colors, outlines, underlines, and
strikethrough in its rack editor. Scope and spectrum show the module's
mix summed for analysis. During rack movement, Cardinal reuses cached
visuals and pauses Strudel preview updates while audio continues.
