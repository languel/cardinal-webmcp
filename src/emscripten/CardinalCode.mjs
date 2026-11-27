// SPDX-License-Identifier: GPL-3.0-or-later

// This function is embedded in a Blob worker inside an opaque-origin frame.
function workerMain() {
  self.addEventListener('message', function initialize({ data, ports }) {
    const [port] = ports;
    const send = port.postMessage.bind(port);
    const finite = Number.isFinite;
    const clamp = value => finite(value) ? Math.max(-10, Math.min(10, value)) : 0;
    let process;
    try {
      process = new Function(
        '"use strict";\n' + data + '\nreturn typeof process === "function" ? process : null;'
      )();
      if (typeof process !== 'function') throw new Error('Define a process({ input, time }) function.');
    } catch (error) {
      send({ error: String(error).slice(0, 500) });
      return;
    }

    port.onmessage = ({ data: { input, time, outputCount } }) => {
      try {
        const output = process({ input, time });
        if (!Array.isArray(output) && (outputCount !== 0 || output !== undefined))
          throw new Error('Return an array of output voltages.');
        send({ output: Array.from({ length: outputCount }, (_, i) => clamp(output[i])) });
      } catch (error) {
        send({ error: String(error).slice(0, 500) });
      }
    };
    send({ ready: true });
  }, { once: true });
}

function frameMain(workerSource) {
  let port;
  addEventListener('message', ({ source, data }) => {
    if (source !== parent) return;
    if (data.kind === 'tick') {
      port?.postMessage(data.tick);
      return;
    }
    if (data.kind !== 'start' || port) return;
    try {
      const blob = URL.createObjectURL(new Blob([workerSource], { type: 'text/javascript' }));
      const worker = new Worker(blob);
      URL.revokeObjectURL(blob);
      const { port1, port2 } = new MessageChannel();
      port = port1;
      port.onmessage = event => parent.postMessage(event.data, '*');
      worker.onerror = () => parent.postMessage({ error: 'The JavaScript worker failed.' }, '*');
      worker.postMessage(data.source, [port2]);
    } catch (error) {
      parent.postMessage({ error: String(error) }, '*');
    }
  });
  parent.postMessage({ frameReady: true }, '*');
}

// No patch source is interpolated into HTML. It is sent after the frame is ready.
const runnerDocument = `<!doctype html>
<meta http-equiv="Content-Security-Policy"
  content="default-src 'none'; script-src 'unsafe-inline' 'unsafe-eval'; worker-src blob:; connect-src 'none'">
<script>(${frameMain})(${JSON.stringify(`(${workerMain})()`)});</script>`;

const visualDependencies = Object.freeze({
  1: 'https://cdn.jsdelivr.net/npm/p5@2.3.3/lib/p5.min.js',
  2: 'https://cdn.jsdelivr.net/npm/three@0.187.0/build/three.module.js',
  playCore: 'https://cdn.jsdelivr.net/gh/ertdfgcvb/play.core@6733888503799c051a43b175e5488608a9775238/src/modules/'
});

// Serialized into a sandboxed iframe. Patch source is sent by message.
function visualFrameMain(dependencies) {
  const send = (message, transfer) => parent.postMessage(message, '*', transfer);
  const report = error => send({ error: String(error?.message || error).slice(0, 500) });
  addEventListener('error', event => report(event.error || event.message));
  addEventListener('unhandledrejection', event => report(event.reason));

  const rack = {
    width: 640, height: 320, input: [], output: [], time: 0, deltaTime: 0,
    pointer: { x: 0, y: 0, down: false }
  };
  const hostCanvas = document.createElement('canvas');
  document.body.append(hostCanvas);
  let draw;
  let previousTime = 0;

  function loadScript(url) {
    return new Promise((resolve, reject) => {
      const script = document.createElement('script');
      script.onload = resolve;
      script.onerror = () => reject(new Error('Could not load p5.js.'));
      script.crossOrigin = 'anonymous';
      script.src = url;
      document.head.append(script);
    });
  }

  async function setupP5(source) {
    await loadScript(dependencies[1]);
    p5.disableFriendlyErrors = true;
    const code = new Function('rack', source + '\nreturn {'
      + 'setup: typeof setup === "function" ? setup : null,'
      + 'draw: typeof draw === "function" ? draw : null,'
      + 'sketch: typeof sketch === "function" ? sketch : null'
      + '};')(rack);
    return new Promise((resolve, reject) => {
      function configure(instance, style) {
        return async function () {
          try {
            instance.pixelDensity(1);
            await style?.();
            if (!instance.canvas) instance.createCanvas(rack.width, rack.height);
            instance.noLoop();
            const followsPreview = instance.width === rack.width && instance.height === rack.height;
            resolve(async () => {
              if (followsPreview && (instance.width !== rack.width || instance.height !== rack.height))
                instance.resizeCanvas(rack.width, rack.height, true);
              instance.pmouseX = instance.mouseX;
              instance.pmouseY = instance.mouseY;
              instance.mouseX = rack.pointer.x * instance.width;
              instance.mouseY = rack.pointer.y * instance.height;
              instance.mouseIsPressed = rack.pointer.down;
              await instance.redraw();
              return instance.canvas;
            });
          } catch (error) {
            reject(error);
          }
        };
      }
      if (code.sketch) {
        new p5(instance => {
          code.sketch(instance);
          instance.setup = configure(instance, instance.setup?.bind(instance));
        });
      } else {
        if (!code.draw) throw new Error('Define draw() or sketch(p).');
        window.setup = () => configure(p5.instance, code.setup)();
        window.draw = code.draw;
        new p5();
      }
    });
  }

  async function importSketch(source) {
    const blob = URL.createObjectURL(new Blob([source], { type: 'text/javascript' }));
    try {
      return await import(blob);
    } finally {
      URL.revokeObjectURL(blob);
    }
  }

  async function setupThree(source) {
    const THREE = await import(dependencies[2]);
    const renderer = new THREE.WebGLRenderer({
      canvas: hostCanvas, antialias: true, alpha: true, preserveDrawingBuffer: true
    });
    renderer.setSize(rack.width, rack.height, false);
    Object.assign(window, { THREE, renderer, rack });
    const module = await importSketch(source + '\nexport const __cardinalHooks = {'
      + 'setup: typeof setup === "function" ? setup : null,'
      + 'draw: typeof draw === "function" ? draw : null };');
    const program = { ...module.__cardinalHooks, ...module };
    if (!program.draw) throw new Error('Define a draw() function and call renderer.render(scene, camera).');
    await program.setup?.({ THREE, renderer, rack });
    return async () => {
      if (hostCanvas.width !== rack.width || hostCanvas.height !== rack.height)
        renderer.setSize(rack.width, rack.height, false);
      await program.draw({ THREE, renderer, rack });
      return hostCanvas;
    };
  }

  async function setupAscii(source) {
    Object.assign(window, { rack, __: rack });
    const compatibleSource = source.replace(
      /(\bfrom\s+["'])(?:\.{1,2}\/|\/)?(?:src\/)?modules\/([a-zA-Z][\w-]*\.js)(["'])/g,
      (_, prefix, file, quote) => prefix + dependencies.playCore + file + quote
    );
    const program = await importSketch(compatibleSource);
    if (typeof program.main !== 'function') throw new Error('Export a Play.core main(coord, context) function.');
    const settings = program.settings || {};
    const context = { time: 0, frame: 0, cols: 0, rows: 0, rack, settings };
    const cursor = { x: 0, y: 0, down: false, pressed: false };
    const graphics = hostCanvas.getContext('2d');
    let lastTime = -Infinity;
    const resolveSize = (value, fallback, max) => Math.min(max,
      Math.max(1, Math.floor(Number(value)) || fallback));
    const gridSize = () => {
      context.cols = resolveSize(settings.cols, Math.floor(rack.width / 10), 128);
      context.rows = resolveSize(settings.rows, Math.floor(rack.height / 15), 72);
      context.width = rack.width;
      context.height = rack.height;
      const cellWidth = rack.width / context.cols;
      const cellHeight = rack.height / context.rows;
      context.metrics = { cellWidth, cellHeight, aspect: cellWidth / cellHeight };
    };
    gridSize();
    await program.boot?.(rack);
    return async () => {
      const fps = resolveSize(settings.fps, 30, 60);
      if (rack.time * 1000 - lastTime < 1000 / fps - 1) return hostCanvas;
      lastTime = rack.time * 1000;
      if (hostCanvas.width !== rack.width || hostCanvas.height !== rack.height) {
        hostCanvas.width = rack.width;
        hostCanvas.height = rack.height;
      }
      gridSize();
      context.time = rack.time * 1000;
      context.frame++;
      const x = Math.min(context.cols - 1, Math.floor(rack.pointer.x * context.cols));
      const y = Math.min(context.rows - 1, Math.floor(rack.pointer.y * context.rows));
      const moved = x !== cursor.x || y !== cursor.y;
      const changed = rack.pointer.down !== cursor.down;
      const p = { x: cursor.x, y: cursor.y, pressed: cursor.pressed };
      Object.assign(cursor, { x, y, p, down: rack.pointer.down, pressed: rack.pointer.down });
      const { cols, rows } = context;
      const buffer = Array.from({ length: cols * rows }, () => ({ char: ' ' }));
      program.pre?.(context, cursor, buffer, rack);
      const pointerCallback = changed ? (cursor.down ? program.pointerDown : program.pointerUp)
        : moved ? program.pointerMove : null;
      pointerCallback?.(context, cursor, buffer, rack);
      for (let y = 0; y < rows; ++y) {
        for (let x = 0; x < cols; ++x) {
          const index = x + y * cols;
          const cell = program.main({ x, y, index }, context, cursor, buffer, rack);
          if (cell !== undefined)
            Object.assign(buffer[index], cell && typeof cell === 'object' ? cell : { char: cell });
        }
      }
      program.post?.(context, cursor, buffer, rack);
      const cellWidth = rack.width / cols;
      const cellHeight = rack.height / rows;
      graphics.fillStyle = settings.backgroundColor || '#11131a';
      graphics.fillRect(0, 0, rack.width, rack.height);
      graphics.font = Math.floor(Math.min(cellHeight, cellWidth * 1.58)) + 'px monospace';
      graphics.textAlign = 'center';
      graphics.textBaseline = 'middle';
      for (const [index, cell] of buffer.entries()) {
        const x = index % cols * cellWidth;
        const y = Math.floor(index / cols) * cellHeight;
        if (cell.backgroundColor) {
          graphics.fillStyle = cell.backgroundColor;
          graphics.fillRect(x, y, cellWidth, cellHeight);
        }
        if (cell.char && cell.char !== ' ') {
          graphics.fillStyle = cell.color || settings.color || '#e8e8e8';
          graphics.fillText(String(cell.char).slice(0, 8), x + cellWidth / 2, y + cellHeight / 2, cellWidth);
        }
      }
      return hostCanvas;
    };
  }

  addEventListener('message', async ({ source, data }) => {
    if (source !== parent || !data) return;
    try {
      if (data.kind === 'start') {
        Object.assign(rack, data.tick);
        if (data.runtime === 1) draw = await setupP5(data.source);
        else if (data.runtime === 2) draw = await setupThree(data.source);
        else draw = await setupAscii(data.source);
        send({ visualReady: true });
      } else if (data.kind === 'tick' && draw) {
        Object.assign(rack, data.tick);
        rack.deltaTime = rack.time - previousTime;
        rack.output = [];
        previousTime = rack.time;
        const canvas = await draw();
        if (!canvas) throw new Error('The sketch did not create a canvas.');
        const bitmap = await createImageBitmap(canvas, {
          resizeWidth: rack.width, resizeHeight: rack.height, premultiplyAlpha: 'premultiply'
        });
        send({ bitmap, output: rack.output }, [bitmap]);
      }
    } catch (error) {
      report(error);
    }
  });
  send({ frameReady: true });
}

const visualDocument = '<!doctype html><meta http-equiv="Content-Security-Policy"'
  + ' content="default-src \'none\'; script-src \'unsafe-inline\' \'unsafe-eval\' blob:'
  + ' https://cdn.jsdelivr.net/npm/p5@2.3.3/lib/p5.min.js'
  + ' https://cdn.jsdelivr.net/npm/three@0.187.0/build/'
  + ' https://cdn.jsdelivr.net/npm/three@0.187.0/examples/jsm/'
  + ' ' + visualDependencies.playCore + '; connect-src \'none\';'
  + ' style-src \'unsafe-inline\'; img-src data: blob:">'
  + '<script type="importmap">' + JSON.stringify({ imports: {
    three: visualDependencies[2],
    'three/addons/': new URL('../examples/jsm/', visualDependencies[2]).href
  } }) + '</script>'
  + '<body><script>(' + visualFrameMain + ')(' + JSON.stringify(visualDependencies) + ');</script>';

function installVisualRunner() {
  const states = new Map();
  const frames = new Map();
  const finite = value => Number.isFinite(value) ? value : 0;

  function detach(state) {
    clearTimeout(state.timeout);
    if (state.frame) {
      frames.delete(state.frame.contentWindow);
      state.frame.remove();
      state.frame = null;
    }
    state.bitmap?.close();
    state.bitmap = null;
  }

  function fail(state, error) {
    detach(state);
    state.output = [];
    state.error = error;
  }

  function watchdog(state, duration, error) {
    clearTimeout(state.timeout);
    state.timeout = setTimeout(() => fail(state, error), duration);
  }

  window.addEventListener('message', ({ source, data }) => {
    if (data?.visual === true) return;
    const state = frames.get(source);
    if (!state || !data) {
      if (data?.bitmap instanceof ImageBitmap) data.bitmap.close();
      return;
    }
    if (typeof data.error === 'string') {
      fail(state, data.error.slice(0, 500) || 'The sketch failed.');
    } else if (data.frameReady) {
      state.frameReady = true;
    } else if (data.visualReady) {
      clearTimeout(state.timeout);
      state.ready = true;
    } else if (state.pending && data.bitmap instanceof ImageBitmap) {
      clearTimeout(state.timeout);
      state.pending = false;
      state.bitmap?.close();
      state.bitmap = data.bitmap;
      state.output = Array.isArray(data.output) ? data.output.slice(0, 6)
        .map(value => Math.max(-10, Math.min(10, finite(value)))) : [];
    }
  });

  function removeVisual(id) {
    const state = states.get(id);
    if (state) detach(state);
    states.delete(id);
  }

  return {
    runVisual(id, runtime, source) {
      removeVisual(id);
      const frame = document.createElement('iframe');
      frame.sandbox = 'allow-scripts';
      frame.title = 'Rack visual runner';
      frame.hidden = true;
      frame.srcdoc = visualDocument;
      document.body.append(frame);
      const state = { frame, runtime, source, started: performance.now(), output: [], lastTick: -Infinity };
      states.set(id, state);
      frames.set(frame.contentWindow, state);
      watchdog(state, 2000, 'The visual runner did not start.');
    },

    updateVisual(id, input, width, height, x, y, down) {
      const state = states.get(id);
      if (!state) return { error: 'The visual runner closed.', output: [] };
      const now = performance.now();
      const scale = Math.min(1.5, 1152 / width, 640 / height);
      const tick = {
        input: input.map(finite), width: Math.max(1, Math.round(width * scale)),
        height: Math.max(1, Math.round(height * scale)),
        time: (now - state.started) / 1000, pointer: { x, y, down: !!down }
      };
      if (state.frameReady && !state.sentStart && state.frame) {
        state.sentStart = true;
        state.frame.contentWindow.postMessage({
          kind: 'start', runtime: state.runtime, source: state.source, tick
        }, '*');
        watchdog(state, 12000, 'The sketch or its library did not finish loading.');
      } else if (state.ready && state.frame && !state.pending && now - state.lastTick >= 1000 / 30 - 1) {
        state.lastTick = now;
        state.pending = true;
        state.frame.contentWindow.postMessage({ kind: 'tick', tick }, '*');
        watchdog(state, 1500, 'The sketch did not produce a frame.');
      }
      const bitmap = state.bitmap;
      state.bitmap = null;
      return { error: state.error, output: state.output, bitmap };
    },

    removeVisual
  };
}

// Runs alongside the Strudel evaluator in its opaque-origin frame.
function strudelVisuals(core, draw, registerWidgetType, source, send) {
  const proto = core.Pattern.prototype;
  const views = [];
  const spiral = proto.spiral;
  const canvas = new OffscreenCanvas(1, 1);
  const ctx = canvas.getContext('2d');
  const encoder = new TextEncoder();
  const canvasColor = new OffscreenCanvas(1, 1).getContext('2d', { willReadFrequently: true });
  const marksCache = new Map();
  const localClamp = (x, lo, hi) => Math.min(hi, Math.max(lo, x));
  const bounded = (value, fallback, lo, hi) => Number.isFinite(+value) ? localClamp(+value, lo, hi) : fallback;
  let playback;

  const color = css => {
    canvasColor.clearRect(0, 0, 1, 1);
    canvasColor.fillStyle = 'transparent';
    canvasColor.fillStyle = css;
    canvasColor.fillRect(0, 0, 1, 1);
    const bytes = canvasColor.getImageData(0, 0, 1, 1).data;
    return (bytes[0] | bytes[1] << 8 | bytes[2] << 16 | bytes[3] << 24) >>> 0;
  };

  function markStyle(value) {
    const ink = typeof value?.color === 'string' ? value.color : '#a8c5da';
    const css = typeof value?.markcss === 'string' ? value.markcss : '';
    const key = JSON.stringify([ink, css]);
    if (marksCache.has(key)) return marksCache.get(key);
    // Read only editor-relevant declarations. Never attach user CSS to a DOM element.
    const style = document.createElement('span').style;
    style.cssText = css;
    const foreground = style.color ? color(style.color) : 0;
    const background = style.backgroundColor ? color(style.backgroundColor) : 0;
    const outline = style.outlineStyle && style.outlineStyle !== 'none' ? color(style.outlineColor || ink) : 0;
    const decoration = (style.textDecorationLine || style.textDecoration).split(/\s+/);
    const flags = (decoration.includes('underline') ? 1 : 0) | (decoration.includes('line-through') ? 2 : 0);
    const parsed = [foreground, background, css ? outline : color(ink), color(style.textDecorationColor || style.color || ink), flags];
    if (marksCache.size >= 128) marksCache.clear();
    marksCache.set(key, parsed);
    return parsed;
  }

  const locations = new Map();
  function sourceLocation(start, end) {
    const key = `${start}:${end}`;
    if (locations.has(key)) return locations.get(key);
    if (!Number.isInteger(start) || !Number.isInteger(end) || start < 0 || end > source.length || end <= start)
      return null;
    const prefix = source.slice(0, start);
    const token = source.slice(start, end);
    if (token.includes('\n')) return null;
    const line = prefix.split('\n').length - 1;
    const first = encoder.encode(prefix.slice(prefix.lastIndexOf('\n') + 1)).length;
    const location = [line, first, first + encoder.encode(token).length];
    if (locations.size < 2048) locations.set(key, location);
    return location;
  }

  function addView(pattern, kind, options, inline) {
    if (views.length >= 12) return pattern;
    if (!options || typeof options !== 'object') options = {};
    const id = `cardinal-view-${views.length}`;
    const target = new OffscreenCanvas(1, 1);
    const paint = target.getContext('2d');
    const view = { id, kind, options, inline, target, paint };
    if (kind === 'pianoroll') view.pattern = pattern;
    if (kind === 'spiral') {
      view.spiralOptions = { ...options, ctx: paint };
      view.painters = spiral.call(pattern, view.spiralOptions).getPainters();
      // Other decorators on the same pattern are rendered by their own views.
      view.painters = view.painters.slice(-1);
    }
    views.push(view);
    return pattern.tag(id);
  }

  const kinds = ['pianoroll', 'punchcard', 'spiral', 'pitchwheel', 'scope', 'spectrum', 'fscope', 'wordfall'];
  for (const kind of kinds) {
    proto[kind] = function (options) { return addView(this, kind, options, false); };
    registerWidgetType(`_${kind}`);
    proto[`_${kind}`] = function (_id, options) { return addView(this, kind, options, true); };
  }
  proto.tscope = proto.scope;
  registerWidgetType('_tscope');
  proto._tscope = proto._scope;
  proto.markcss = function (css) { return this.set(core.pure({ markcss: String(css ?? '') })); };

  function paintSignal(view, wave, spectrum) {
    const { paint, target: { width, height }, kind, options } = view;
    const { color = '#a8c5da' } = options;
    paint.fillStyle = paint.strokeStyle = color;
    paint.lineWidth = bounded(options.thickness, 2, .5, 16);
    if (kind === 'scope') {
      const pos = bounded(options.pos, .5, 0, 1);
      const scale = bounded(options.scale, .35, 0, 4);
      const trigger = bounded(options.trigger, 0, -1, 1);
      let first = 0;
      if (options.align !== false && wave) {
        first = Math.max(0, wave.findIndex((v, i) => i && wave[i - 1] > -trigger && v <= -trigger));
      }
      paint.beginPath();
      const samples = Math.min(wave?.length / 2 || 0, (wave?.length || 0) - first);
      if (samples) {
        for (let i = 0; i < samples; ++i) {
          const x = i * width / (samples - 1);
          const y = (pos - scale * wave[first + i]) * height;
          if (!i) paint.moveTo(x, y);
          else paint.lineTo(x, y);
        }
      } else {
        paint.moveTo(0, pos * height);
        paint.lineTo(width, pos * height);
      }
      paint.stroke();
    } else if (spectrum) {
      const min = bounded(options.min, -80, -160, -1);
      const max = bounded(options.max, 0, min + 1, 20);
      const level = db => localClamp((db - min) / (max - min), 0, 1);
      if (kind === 'spectrum') {
        const step = Math.min(width, bounded(options.speed, 2, 1, 24));
        paint.clearRect(0, 0, step, height);
        paint.drawImage(view.target, -step, 0);
        paint.clearRect(width - step, 0, step, height);
        for (let y = 0; y < height; ++y) {
          const bin = Math.floor(Math.pow(spectrum.length, 1 - y / height)) - 1;
          paint.globalAlpha = level(spectrum[bin]);
          paint.fillRect(width - step, y, step, 1);
        }
        paint.globalAlpha = 1;
      } else {
        const scale = bounded(options.scale, .75, 0, 4);
        const pos = bounded(options.pos, .85, 0, 1);
        const lean = bounded(options.lean, 1, 0, 1);
        const step = width / spectrum.length;
        for (let i = 0; i < spectrum.length; ++i) {
          const size = level(spectrum[i]) * height * scale;
          paint.fillRect(i * step, height * pos - size * lean, Math.max(1, step), size);
        }
      }
    }
  }

  const signalKinds = ['scope', 'spectrum', 'fscope'];
  function paintView(view, time, finalPattern, wave, spectrum) {
    const { kind, options, target, paint, id } = view;
    if (kind !== 'spectrum') paint.clearRect(0, 0, target.width, target.height);
    if (signalKinds.includes(kind)) {
      paintSignal(view, wave, spectrum);
      return;
    }
    const cycles = bounded(options.cycles, 4, .25, 16);
    const playhead = bounded(options.playhead, .5, 0, 1);
    const behind = cycles * playhead;
    const ahead = cycles - behind;
    const pattern = view.pattern || finalPattern;
    const haps = pattern.queryArc(time - behind, time + ahead + .001)
      .filter(hap => hap.whole && (view.pattern || hap.hasTag(id))).slice(0, 4096);
    if (kind === 'spiral') {
      view.spiralOptions.size = bounded(options.size, Math.min(target.width, target.height) / 6, 1, 640);
      for (const painter of view.painters) painter(paint, time, haps, [-behind, ahead]);
    } else if (kind === 'pitchwheel') {
      draw.pitchwheel({ ...options, ctx: paint, time, haps: haps.filter(hap => hap.isActive(time)) });
    } else {
      const rollOptions = kind === 'wordfall' ? { vertical: 1, labels: 1, stroke: 0, fillActive: 1 } : {};
      draw.__pianoroll({ ...rollOptions, ...options, id: undefined, ctx: paint, time, haps, cycles, playhead });
    }
  }

  addEventListener('message', async event => {
    if (event.source !== parent || event.data?.kind !== 'strudelVisual' || !playback) return;
    const { width, height, wave, spectrum } = event.data;
    try {
      const { scheduler } = playback;
      const time = scheduler.now();
      const marks = new Map();
      for (const hap of scheduler.pattern.queryArc(Math.max(0, time), Math.max(0, time) + .001)) {
        if (!hap.whole || !hap.isActive(time)) continue;
        for (const { start, end } of hap.context.locations || []) {
          const location = sourceLocation(start, end);
          if (location) marks.set(`${start}:${end}`, [...location, ...markStyle(hap.value)]);
          if (marks.size >= 128) break;
        }
        if (marks.size >= 128) break;
      }

      let bitmap;
      if (views.length && width && height) {
        if (canvas.width !== width) canvas.width = width;
        if (canvas.height !== height) canvas.height = height;
        ctx.clearRect(0, 0, width, height);
        const weights = views.map(view => bounded(view.options.height, view.inline ? 60 : 160, 20, 640));
        const total = weights.reduce((sum, weight) => sum + weight, 0);
        const gap = views.length > 1 ? 4 : 0;
        let y = 0;
        views.forEach((view, index) => {
          const endY = index === views.length - 1 ? height
            : y + Math.floor((height - gap * (views.length - 1)) * weights[index] / total);
          const targetHeight = Math.max(1, endY - y);
          const targetWidth = Math.max(1, Math.round(bounded(view.options.width, width, 1, width)));
          if (view.target.width !== targetWidth) view.target.width = targetWidth;
          if (view.target.height !== targetHeight) view.target.height = targetHeight;
          paintView(view, time, scheduler.pattern, wave, spectrum);
          ctx.drawImage(view.target, (width - targetWidth) / 2, y);
          y = endY + gap;
        });
        bitmap = canvas.transferToImageBitmap();
      }
      const payload = { visual: true, marks: new Uint32Array([...marks.values()].flat()), bitmap };
      parent.postMessage(payload, '*', bitmap ? [bitmap, payload.marks.buffer] : [payload.marks.buffer]);
    } catch (error) {
      send({ visualError: String(error) });
    }
  });

  return {
    start(repl) {
      playback = repl;
      return { views: views.length > 0, signal: views.some(view => signalKinds.includes(view.kind)) };
    }
  };
}

// Source code runs in an opaque-origin frame. Scheduled events reach the
// host audio graph, and visual feedback returns as pixels and editor ranges.
function strudelFrameMain() {
  const send = message => parent.postMessage(message, '*');
  let externalCps = 0;
  let internalCps = .5;
  let repl;
  let visualClock;
  let linked = false;

  function output(hap, duration, cps, timing) {
    const value = hap.value;
    const midiNote = value?.cardinalMidi && typeof value.note === 'string'
      ? coreNoteToMidi(value.note) : value?.note;
    send({ value, midiNote, duration, cps, cycle: Number(hap.whole.begin), ...timing });
  }

  let coreNoteToMidi;
  addEventListener('message', event => {
    if (event.source !== parent || !linked || !repl?.state.pattern) return;
    const data = event.data;
    if (data?.kind === 'strudelTransportStopped') {
      visualClock = null;
    } else if (data?.kind === 'strudelSchedule') {
      const { from, to, origin, secondsPerBeat, beatsPerBar, zero, audioNow, epoch } = data;
      visualClock = { origin, secondsPerBeat, beatsPerBar, zero, audioNow, wall: performance.now() / 1000 };
      try {
        const secondsPerCycle = secondsPerBeat * beatsPerBar;
        for (const hap of repl.state.pattern.queryArc((from - origin) / beatsPerBar, (to - origin) / beatsPerBar)) {
          if (!hap.hasOnset()) continue;
          const beat = origin + Number(hap.whole.begin) * beatsPerBar;
          output(hap, Number(hap.duration) * secondsPerCycle, 1 / secondsPerCycle,
            { audioTime: zero + beat * secondsPerBeat, epoch });
        }
      } catch (error) {
        send({ error: String(error) });
      }
    }
  });

  addEventListener('message', event => {
    if (event.source !== parent || linked || event.data?.kind !== 'strudelTempo') return;
    externalCps = event.data.cps;
    repl?.setCps(externalCps || internalCps);
  });
  addEventListener('message', async event => {
    if (event.source !== parent || typeof event.data?.source !== 'string') return;
    try {
      const [core, mini, { transpiler, registerWidgetType }, draw] = await Promise.all([
        import('https://esm.sh/@strudel/core@1.2.6'),
        import('https://esm.sh/@strudel/mini@1.2.6'),
        import('https://esm.sh/@strudel/transpiler@1.2.6'),
        import('https://esm.sh/@strudel/draw@1.2.6')
      ]);
      linked = event.data.linked === true;
      coreNoteToMidi = core.noteToMidi;
      const visuals = strudelVisuals(core, draw, registerWidgetType, event.data.source, send);
      core.Pattern.prototype.midi = function (port, options = {}) {
        return this.set(core.pure({
          cardinalMidi: true, cardinalMidiPort: port ?? 0,
          cardinalMidiChannel: options.midichannel ?? 1, cardinalMidiVelocity: options.velocity ?? .9
        }));
      };
      await core.evalScope(core, mini, draw);
      repl = core.repl({
        transpiler,
        getTime: () => performance.now() / 1000,
        onUpdateState: state => {
          if (state.error) send({ error: String(state.error) });
        },
        beforeStart: () => {
          internalCps = repl.scheduler.cps;
          if (externalCps) repl.setCps(externalCps);
        },
        defaultOutput: (hap, _deadline, duration, cps, time) => {
          output(hap, duration, cps, { time: performance.timeOrigin / 1000 + time });
        }
      });
      if (linked) {
        repl.scheduler.now = () => {
          if (!visualClock) return 0;
          const { audioNow, wall, zero, origin, secondsPerBeat, beatsPerBar } = visualClock;
          const current = audioNow + performance.now() / 1000 - wall;
          return Math.max(0, ((current - zero) / secondsPerBeat - origin) / beatsPerBar);
        };
      }
      if (await repl.evaluate(event.data.source, !linked)) send({ started: true, ...visuals.start(repl) });
    } catch (error) {
      send({ error: String(error) });
    }
  }, { once: true });
  send({ ready: true });
}

function installStrudelRunner(module) {
  const channels = 4;
  const states = new Map();
  const frames = new Map();
  let audio;
  let library;
  let midiAccess;
  let midiError = '';
  let transport = { enabled: false, playing: false, beat: 0, bpm: 120, grid: 4, beatsPerBar: 4, revision: 0, zero: null };
  const lookahead = .20;
  const launchLead = .075;

  function unlock() {
    if (audio?.state === 'suspended') audio.resume().catch(() => {});
  }
  window.addEventListener('pointerdown', unlock, true);
  window.addEventListener('keydown', unlock, true);

  function closeFrame(runner) {
    clearTimeout(runner.timeout);
    if (!runner.frame) return;
    frames.delete(runner.frame.contentWindow);
    runner.frame.remove();
    runner.frame = null;
  }

  function discardReplacement(state) {
    if (state.replacement) closeFrame(state.replacement);
    state.replacement = null;
  }

  function detach(state) {
    discardReplacement(state);
    closeFrame(state);
    if (state.processor) {
      state.processor.onaudioprocess = null;
      state.mix.disconnect();
      state.gate.disconnect();
      state.input.disconnect();
      state.processor.disconnect();
      state.processor = null;
    }
    state.bitmap?.close();
    state.bitmap = null;
    for (const note of state.midiNotes || []) {
      if (note.off > performance.now()) {
        try { note.port.send(note.message, Math.max(performance.now(), note.onset + 1)); }
        catch (error) { console.warn('Strudel MIDI note-off:', error); }
      }
    }
    state.midiNotes = [];
    state.blocks.length = 0;
    state.running = false;
  }

  function fail(state, error) {
    detach(state);
    state.error = String(error).slice(0, 500);
  }

  function removeStrudel(id) {
    const state = states.get(id);
    if (state) detach(state);
    states.delete(id);
  }

  function openFrame(state, runner) {
    const frame = document.createElement('iframe');
    frame.sandbox = 'allow-scripts';
    frame.title = 'Rack Strudel runner';
    frame.hidden = true;
    frame.srcdoc = `<!doctype html><meta http-equiv="Content-Security-Policy"
      content="default-src 'none'; script-src 'unsafe-inline' 'unsafe-eval' https://esm.sh">
      <body><script>${strudelVisuals.toString()};(${strudelFrameMain.toString()})()<\/script></body>`;
    document.body.append(frame);
    runner.frame = frame;
    frames.set(frame.contentWindow, state);
  }

  function setViews(state, runner) {
    state.views = runner.views;
    if (runner.signal && !state.analyser) {
      state.analyser = audio.createAnalyser();
      state.analyser.fftSize = 1024;
      state.gate.connect(state.analyser);
    } else if (!runner.signal && state.analyser) {
      state.gate.disconnect(state.analyser);
      state.analyser = null;
    }
  }

  function replacementError(state, error) {
    if (state.replacement?.scheduled)
      state.next = Math.min(state.next, state.replacement.beat);
    discardReplacement(state);
    state.updateError = String(error).slice(0, 500);
  }

  function prepareReplacement(state, source) {
    discardReplacement(state);
    const replacement = state.replacement = { source };
    state.updateError = '';
    replacement.timeout = setTimeout(() => replacementError(state, 'Strudel update timed out.'), 15000);
    try {
      openFrame(state, replacement);
    } catch (error) {
      replacementError(state, error);
    }
  }

  function activateReplacement(state, replacement) {
    closeFrame(state);
    state.frame = replacement.frame;
    state.source = replacement.source;
    state.sourceRevision++;
    state.replacement = null;
    state.bitmap?.close();
    // A replacement with no graphics still has to clear the previous texture.
    state.bitmap = state.views && !replacement.views && typeof OffscreenCanvas === 'function'
      ? new OffscreenCanvas(1, 1).transferToImageBitmap() : null;
    state.marks = null;
    state.visualError = '';
    state.visualPending = false;
    state.lastVisual = undefined;
    setViews(state, replacement);
    if (state.nextSource !== undefined) {
      const source = state.nextSource;
      state.nextSource = undefined;
      prepareReplacement(state, source);
    }
  }

  function createAudioGraph(state, dough) {
    const Controller = dough.getSuperdoughAudioController().constructor;
    state.controller = new Controller(audio);
    state.controller.output.destinationGain.disconnect();
    const mix = audio.createChannelMerger(channels);
    state.mix = mix;
    state.input = audio.createGain();
    state.inputCursor = 0;
    state.controller.output.connectToDestination = (source, destinations = [0, 1]) => {
      const stereo = audio.createStereoPanner();
      const splitter = audio.createChannelSplitter(2);
      source.connect(stereo).connect(splitter);
      destinations.slice(0, channels).forEach((channel, i) => {
        if (Number.isInteger(channel) && channel >= 0 && channel < channels)
          splitter.connect(mix, i % 2, channel);
      });
    };
    const processor = audio.createScriptProcessor(1024, channels, 1);
    state.processor = processor;
    state.gate = audio.createGain();
    state.gate.channelCount = channels;
    state.gate.channelCountMode = 'explicit';
    state.gate.gain.value = state.linked ? 0 : 1;
    mix.connect(state.gate).connect(processor);
    processor.connect(audio.destination);
    // This capture node emits silence. Only Host Audio plays the rack mix.
    processor.onaudioprocess = ({ inputBuffer }) => {
      if (!state.running) return;
      if (state.blocks.length === 8) {
        state.blocks.shift();
        state.position = 0;
      }
      state.blocks.push(Array.from({ length: channels }, (_, channel) =>
        inputBuffer.getChannelData(channel).slice()));
    };
  }

  function addRackInput(state, pcm, rate) {
    if (!pcm.length || !state.hasRackInput) return;
    const buffer = audio.createBuffer(2, pcm.length / 2, rate);
    for (let channel = 0; channel < 2; ++channel) {
      const target = buffer.getChannelData(channel);
      for (let i = 0; i < target.length; ++i) target[i] = pcm[i * 2 + channel];
    }
    const block = audio.createBufferSource();
    block.buffer = buffer;
    block.connect(state.input);
    const time = audio.currentTime;
    if (state.inputCursor < time || state.inputCursor > time + .25)
      state.inputCursor = time + .085;
    block.start(state.inputCursor);
    state.inputCursor += buffer.duration;
  }

  function rackInputSound(state, time, value, duration) {
    state.hasRackInput = true;
    const envelope = audio.createGain();
    envelope.gain.value = 0;
    const end = time + duration;
    const attack = Math.min(duration * .5, Math.max(.002, Number(value.attack) || .005));
    const release = Math.min(3, Math.max(.005, Number(value.release) || .02));
    envelope.gain.setValueAtTime(0, time);
    envelope.gain.linearRampToValueAtTime(1, time + attack);
    envelope.gain.setValueAtTime(1, end);
    envelope.gain.linearRampToValueAtTime(0, end + release);
    state.input.connect(envelope);
    // Use the audio clock to disconnect completed voices, even if UI work is delayed.
    const lifetime = audio.createConstantSource();
    lifetime.start(time);
    lifetime.onended = () => {
      if (state.processor) state.input.disconnect(envelope);
      envelope.disconnect();
    };
    lifetime.stop(end + release);
    return envelope;
  }

  function sendMidi(state, event, delay) {
    const value = event.value;
    const ports = midiAccess && [...midiAccess.outputs.values()];
    const target = value.midiport ?? value.cardinalMidiPort;
    const port = typeof target === 'number' ? ports?.[target]
      : ports?.find(port => port.id === target || port.name === target);
    if (!port) {
      state.midiMessage = midiError || (midiAccess ? 'MIDI output unavailable.' : 'Enable MIDI output from the module menu.');
      return;
    }
    state.midiMessage = '';
    const note = Math.round(Number(event.midiNote));
    if (!Number.isInteger(note) || note < 0 || note > 127) return;
    const channel = Math.min(16, Math.max(1, Math.round(Number(value.midichan ?? value.cardinalMidiChannel) || 1))) - 1;
    const velocity = Math.min(127, Math.max(1, Math.round((Number(value.velocity ?? value.cardinalMidiVelocity) || .9) * 127)));
    const onset = performance.now() + Math.max(0, delay * 1000) + 34;
    const off = onset + Math.max(10, Math.min(event.duration * 1000, 10000));
    const message = [0x80 | channel, note, 0];
    port.send([0x90 | channel, note, velocity], onset);
    port.send(message, off);
    state.midiNotes = (state.midiNotes || []).filter(note => note.off > performance.now());
    state.midiNotes.push({ port, message, onset, off });
  }

  window.addEventListener('message', ({ source, data }) => {
    const state = frames.get(source);
    if (!state || !data) {
      if (data?.visual === true && data.bitmap instanceof ImageBitmap) data.bitmap.close();
      return;
    }
    const replacement = source === state.replacement?.frame?.contentWindow ? state.replacement : null;
    if (data.ready === true) {
      source.postMessage({ source: (replacement ?? state).source, linked: state.linked }, '*');
    } else if (typeof data.error === 'string') {
      if (replacement) replacementError(state, data.error);
      else fail(state, data.error);
    } else if (data.started === true) {
      const runner = replacement ?? state;
      clearTimeout(runner.timeout);
      runner.views = data.views === true;
      runner.signal = data.signal === true;
      if (replacement) {
        replacement.ready = true;
      } else {
        state.running = true;
        setViews(state, runner);
      }
    } else if (data.visual === true) {
      if (replacement) {
        if (data.bitmap instanceof ImageBitmap) data.bitmap.close();
        return;
      }
      state.visualPending = false;
      state.bitmap?.close();
      state.bitmap = data.bitmap instanceof ImageBitmap ? data.bitmap : null;
      state.marks = data.marks instanceof Uint32Array ? data.marks.subarray(0, 1024) : null;
    } else if (typeof data.visualError === 'string') {
      if (replacement) return;
      state.visualPending = false;
      state.visualError = data.visualError.slice(0, 500);
    } else if (data.value && typeof data.value === 'object' && !Array.isArray(data.value)) {
      const now = (performance.timeOrigin + performance.now()) / 1000;
      if (state.linked && (!transport.playing || data.epoch !== state.epoch
          || !Number.isFinite(data.audioTime) || data.audioTime >= (state.stopAudioTime ?? Infinity))) return;
      if (replacement && !replacement.scheduled) return;
      const delay = state.linked ? data.audioTime - audio.currentTime : data.time - now;
      if (!Number.isFinite(delay) || delay < (state.linked ? 0 : -.1) || delay > 1 || audio.state !== 'running'
          || !Number.isFinite(data.duration) || !Number.isFinite(data.cps) || !Number.isFinite(data.cycle)) return;
      if (now - state.eventWindow > 1) {
        state.eventWindow = now;
        state.eventCount = 0;
      }
      if (++state.eventCount > 256) return;
      if (data.value.cardinalMidi === true) {
        try { sendMidi(state, data, delay); }
        catch (error) { state.midiMessage = String(error); }
        return;
      }
      const duration = Math.min(10, Math.max(0, data.duration));
      const time = state.linked ? data.audioTime : audio.currentTime + Math.max(.025, delay + .025);
      if (data.value.s === 'rack') {
        data.value.source = (start, value) => rackInputSound(state, start, value, duration);
      }
      state.dough.setSuperdoughAudioController(state.controller);
      state.dough.superdough(data.value, time, duration, data.cps, data.cycle).catch(error => {
        if (replacement && state.replacement === replacement) replacementError(state, error);
        else if (source === state.frame?.contentWindow) fail(state, error);
      });
    }
  });

  function nextBoundary(beat, secondsPerBeat) {
    const earliest = beat + launchLead / secondsPerBeat;
    return transport.grid ? Math.ceil(earliest / transport.grid) * transport.grid : earliest;
  }

  function nextUnscheduledBoundary(state, now, secondsPerBeat) {
    const beat = (now - transport.zero) / secondsPerBeat;
    return nextBoundary(Math.max(beat, state.next - launchLead / secondsPerBeat), secondsPerBeat);
  }

  function setGate(state, on, time) {
    const gain = state.gate.gain;
    gain.cancelScheduledValues(time);
    gain.setValueAtTime(on ? 0 : 1, time);
    gain.linearRampToValueAtTime(on ? 1 : 0, time + .004);
  }

  function synchronize(state, restarting, tempoChanged) {
    if (!state.running || !state.linked || !state.frame || !audio || audio.state !== 'running') return;
    const secondsPerBeat = 60 / transport.bpm;
    const now = audio.currentTime;
    if (!transport.enabled || !transport.playing) {
      if (state.stopBeat !== undefined) {
        detach(state);
        state.finished = true;
        return;
      }
      if (state.origin !== undefined) {
        state.epoch++;
        state.origin = state.next = undefined;
        state.gate.gain.cancelScheduledValues(now);
        state.gate.gain.setValueAtTime(0, now);
        state.blocks.length = 0;
        state.position = 0;
        state.frame.contentWindow.postMessage({ kind: 'strudelTransportStopped' }, '*');
      }
      if (state.replacement?.ready) activateReplacement(state, state.replacement);
      state.armedWhileStopped = true;
      return;
    }
    if (transport.zero === null) return;

    if (restarting || state.origin === undefined) {
      state.epoch++;
      const beat = Math.max(0, (now - transport.zero) / secondsPerBeat);
      const downbeat = (state.armedWhileStopped || restarting) && transport.beat < .15;
      state.origin = downbeat ? 0 : nextBoundary(beat, secondsPerBeat);
      state.armedWhileStopped = false;
      state.next = state.origin;
      if (state.stopBeat !== undefined) {
        state.stopBeat = state.origin + transport.grid;
        state.stopAudioTime = undefined;
      }
      if (state.replacement) {
        state.replacement.beat = undefined;
        state.replacement.scheduled = false;
      }
      state.startAudioTime = transport.zero + state.origin * secondsPerBeat;
      state.gate.gain.cancelScheduledValues(now);
      state.gate.gain.setValueAtTime(0, now);
      setGate(state, true, state.startAudioTime);
    } else if (tempoChanged && state.startAudioTime > now) {
      state.gate.gain.cancelScheduledValues(now);
      state.startAudioTime = transport.zero + state.origin * secondsPerBeat;
      setGate(state, true, Math.max(now, state.startAudioTime));
    }
    if (state.stopBeat !== undefined) {
      const stopTime = transport.zero + state.stopBeat * secondsPerBeat;
      if (state.stopAudioTime === undefined || (tempoChanged && state.stopAudioTime > now)) {
        if (state.stopAudioTime !== undefined)
          state.gate.gain.cancelScheduledValues(state.stopAudioTime);
        state.stopAudioTime = stopTime;
        setGate(state, false, Math.max(now, stopTime));
      }
      if (now >= state.stopAudioTime + .18) {
        detach(state);
        state.finished = true;
        return;
      }
    }

    let replacement = state.replacement;
    if (replacement?.ready && replacement.beat === undefined)
      replacement.beat = nextUnscheduledBoundary(state, now, secondsPerBeat);
    if (replacement?.scheduled && now >= transport.zero + replacement.beat * secondsPerBeat) {
      activateReplacement(state, replacement);
      replacement = null;
    }

    const from = Math.max(state.next, (now + .005 - transport.zero) / secondsPerBeat);
    const to = Math.min((now + lookahead - transport.zero) / secondsPerBeat, state.stopBeat ?? Infinity);
    if (to > from) {
      state.next = to;
      const send = (frame, start, end) => {
        if (end <= start) return;
        frame.contentWindow.postMessage({ kind: 'strudelSchedule', from: start, to: end, origin: state.origin,
          secondsPerBeat, beatsPerBar: transport.beatsPerBar, zero: transport.zero, audioNow: now, epoch: state.epoch }, '*');
      };
      const boundary = replacement?.beat ?? Infinity;
      send(state.frame, from, Math.min(to, boundary));
      if (to > boundary) {
        replacement.scheduled = true;
        send(replacement.frame, Math.max(from, boundary), to);
      }
    }
  }

  return {
    enableStrudelMidi() {
      if (!navigator.requestMIDIAccess) {
        midiError = 'Web MIDI is unavailable in this browser.';
        return;
      }
      navigator.requestMIDIAccess().then(access => {
        midiAccess = access;
        midiError = '';
        for (const state of states.values()) state.midiMessage = '';
      }).catch(error => { midiError = String(error); });
    },

    updateTransport(enabled, playing, beat, bpm, grid, revision, beatsPerBar = 4) {
      const restarting = transport.revision !== revision;
      const tempoChanged = transport.bpm !== bpm;
      let zero = transport.zero;
      const clock = audio ?? module.WebAudioBridge?.audioContext;
      if (!enabled || !playing || clock?.state !== 'running') zero = null;
      else if (zero === null || restarting || tempoChanged)
        zero = clock.currentTime + launchLead - beat * 60 / bpm;
      transport = { enabled, playing: enabled && playing, beat, bpm, grid, beatsPerBar, revision, zero };
      for (const state of states.values()) synchronize(state, restarting, tempoChanged);
    },

    requestStrudelStop(id) {
      const state = states.get(id);
      if (!state?.linked || !state.running || !transport.playing || !transport.grid || state.origin === undefined
          || audio.currentTime < state.startAudioTime) return false;
      if (state.stopBeat !== undefined) return true;
      if (!state.replacement?.scheduled) discardReplacement(state);
      state.nextSource = undefined;
      state.updateError = '';
      const secondsPerBeat = 60 / transport.bpm;
      state.stopBeat = nextUnscheduledBoundary(state, audio.currentTime, secondsPerBeat);
      synchronize(state, false, false);
      return true;
    },

    queueStrudelUpdate(id, source) {
      const state = states.get(id);
      if (!state?.linked || !state.running || state.finished || state.error) return false;
      if (state.stopBeat !== undefined) {
        if (audio.currentTime >= state.stopAudioTime) return false;
        state.gate.gain.cancelScheduledValues(state.stopAudioTime);
        state.stopBeat = state.stopAudioTime = undefined;
      }
      if (state.replacement?.scheduled) state.nextSource = source;
      else prepareReplacement(state, source);
      return true;
    },

    runStrudel(id, source, rate, linked) {
      removeStrudel(id);
      const state = { source, blocks: [], position: 0, eventWindow: 0, eventCount: 0, running: false,
        linked, epoch: 0, sourceRevision: 1, armedWhileStopped: linked && !transport.playing };
      states.set(id, state);
      try {
        audio ??= module.WebAudioBridge?.audioContext ?? new AudioContext({ sampleRate: rate });
        unlock();
      } catch (error) {
        fail(state, error);
        return;
      }
      state.timeout = setTimeout(() => fail(state, 'Strudel did not finish loading.'), 15000);
      library ??= import('https://esm.sh/superdough@1.3.0').then(async dough => {
        dough.setAudioContext(audio);
        await Promise.all([dough.registerSynthSounds(), dough.registerZZFXSounds(), dough.initAudio()]);
        // The built-in synths work even if the optional drum map is unavailable.
        dough.samples('https://raw.githubusercontent.com/felixroos/dough-samples/main/tidal-drum-machines.json')
          .catch(error => console.warn('Strudel drum map unavailable:', error));
        return dough;
      }).catch(error => { library = null; throw error; });
      library.then(dough => {
        if (states.get(id) !== state || state.error) return;
        state.dough = dough;
        createAudioGraph(state, dough);
        openFrame(state, state);
      }).catch(error => {
        if (states.get(id) === state) fail(state, error);
      });
    },

    strudelStatus(id) {
      const state = states.get(id);
      if (!state || state.error) return { error: state?.error || 'The Strudel runner closed.' };
      if (state.finished) return { finished: true };
      if (!state.running) return {};
      if (audio.state !== 'running') return { waitingForGesture: true };
      const applied = { source: state.source, revision: state.sourceRevision };
      if (state.linked) {
        if (!transport.enabled || !transport.playing)
          return { ...applied, pending: state.updateError || 'Armed for transport' };
        if (state.stopBeat !== undefined)
          return { ...applied, running: audio.currentTime < state.stopAudioTime, pending: 'Stop queued' };
        if (state.origin === undefined || audio.currentTime < state.startAudioTime)
          return { ...applied, pending: state.updateError || 'Start queued' };
      }
      return { ...applied, running: true, pending: state.updateError || (state.replacement ? 'Update queued' : '') };
    },

    // Called from the Rack audio processing path, independently of animation.
    transferStrudel(id, output, input, rate, cps) {
      const state = states.get(id);
      if (!state || state.error || !state.running || audio.state !== 'running') return 0;
      if (!state.linked && (state.cps === undefined || Boolean(cps) !== Boolean(state.cps)
          || Math.abs(state.cps - cps) > cps * .005)) {
        state.cps = cps;
        state.frame.contentWindow.postMessage({ kind: 'strudelTempo', cps }, '*');
      }
      addRackInput(state, input, rate);
      const stride = audio.sampleRate / rate;
      let count = 0;
      while (count < output.length / channels && state.blocks.length) {
        const block = state.blocks[0];
        const length = block[0].length;
        const index = Math.floor(state.position);
        if (index >= length) {
          state.position -= length;
          state.blocks.shift();
          continue;
        }
        const next = index + 1;
        const fraction = state.position - index;
        if (fraction && next === length && state.blocks.length === 1) break;
        for (let channel = 0; channel < channels; ++channel) {
          const current = block[channel][index];
          const following = next < length ? block[channel][next] : state.blocks[1]?.[channel][0];
          output[count * channels + channel] = fraction ? current + (following - current) * fraction : current;
        }
        state.position += stride;
        ++count;
      }
      return count;
    },

    updateStrudelVisual(id, width, height) {
      const state = states.get(id);
      if (!state?.running) return {};
      const now = performance.now();
      if (!state.visualError && now - (state.lastVisual ?? -Infinity) >= (state.visualPending ? 1000 : 32)) {
        state.lastVisual = now;
        state.visualPending = true;
        let wave, spectrum;
        if (state.analyser) {
          wave = new Float32Array(state.analyser.fftSize);
          spectrum = new Float32Array(state.analyser.frequencyBinCount);
          state.analyser.getFloatTimeDomainData(wave);
          state.analyser.getFloatFrequencyData(spectrum);
        }
        const scale = Math.min(1.5, 1152 / width, 640 / height);
        state.frame.contentWindow.postMessage({ kind: 'strudelVisual',
          width: state.views ? Math.max(1, Math.round(width * scale)) : 0,
          height: state.views ? Math.max(1, Math.round(height * scale)) : 0, wave, spectrum
        }, '*', wave ? [wave.buffer, spectrum.buffer] : []);
      }
      return { bitmapReady: !!state.bitmap, marks: state.marks, error: state.visualError || state.midiMessage };
    },

    takeStrudelBitmap(id) {
      const state = states.get(id);
      if (!state) return null;
      const bitmap = state.bitmap;
      state.bitmap = null;
      return bitmap;
    },

    removeStrudel
  };
}

async function formatSource(source, glsl) {
  if (!source.trim()) return source;
  const prettier = await import('https://cdn.jsdelivr.net/npm/prettier@3.8.3/standalone.mjs');
  if (!glsl) {
    const plugins = await Promise.all([
      import('https://cdn.jsdelivr.net/npm/prettier@3.8.3/plugins/babel.mjs'),
      import('https://cdn.jsdelivr.net/npm/prettier@3.8.3/plugins/estree.mjs')
    ]);
    return prettier.format(source, { parser: 'babel', plugins, tabWidth: 2 });
  }
  const plugin = await import('https://esm.sh/prettier-plugin-glsl@0.3.1?bundle&deps=prettier@3.8.3');
  const options = { parser: 'glsl-parser', plugins: [plugin], tabWidth: 2 };
  try {
    return await prettier.format(source, options);
  } catch (error) {
    // Twigl snippets can be a function body instead of a translation unit.
    if (/^\s*#/m.test(source) || /\bvoid\s+(?:main|mainImage)\s*\(/.test(source)) throw error;
    const wrapped = await prettier.format('void cardinalFormatBody() {\n' + source + '\n}', options);
    const body = wrapped.match(/^void cardinalFormatBody\(\) \{\n([\s\S]*)\n\}\s*$/);
    if (!body) throw error;
    return body[1].replace(/^ {2}/gm, '') + '\n';
  }
}

export function installCodeRunner(module) {
  // Values match RunStatus in the rack widget.
  const starting = 1;
  const running = 2;
  const failed = 3;
  const states = new Map();
  const frames = new Map();
  let paste;
  let selectAll = 0;
  const editorCommands = new Map();
  const formatJobs = new Map();

  function focusedEditor() {
    return document.activeElement === module.canvas ? module.cardinalCodeEditor : 0;
  }

  // Leave the browser's paste shortcut intact while skipping the canvas's
  // simulated clipboard. Only the selected rack editor receives the result.
  window.addEventListener('keydown', event => {
    const editor = focusedEditor();
    if (!editor) return;
    const commandModifier = event.ctrlKey !== event.metaKey;
    const altOnly = event.altKey && !event.ctrlKey && !event.metaKey;
    const command = altOnly && !event.shiftKey && event.code === 'KeyZ' ? 1
      : commandModifier && !event.altKey && event.code === 'KeyZ' ? (event.shiftKey ? 3 : 2)
      : altOnly && event.shiftKey && event.code === 'KeyF' ? 4 : 0;
    if (command) {
      event.preventDefault();
      event.stopImmediatePropagation();
      if (!event.repeat || command === 2 || command === 3) {
        const pending = editorCommands.get(editor) || [];
        pending.push(command);
        editorCommands.set(editor, pending);
      }
      return;
    }
    if (!commandModifier || event.altKey) return;
    if (event.code === 'KeyA' && !event.shiftKey) {
      event.preventDefault();
      event.stopImmediatePropagation();
      selectAll = editor;
    } else if (event.code === 'KeyV') {
      event.stopImmediatePropagation();
    }
  }, true);
  window.addEventListener('paste', event => {
    const editor = focusedEditor();
    if (!editor || !event.clipboardData) return;
    event.preventDefault();
    const text = event.clipboardData.getData('text/plain');
    paste = { editor, text: text.slice(0, 16384) };
  });


  function detach(state) {
    clearTimeout(state.timeout);
    frames.delete(state.frame.contentWindow);
    state.frame.remove();
  }

  function fail(state, error) {
    detach(state);
    state.frame = null;
    state.output = [0, 0];
    state.status = failed;
    state.error = error || 'The JavaScript worker failed.';
  }

  function watchdog(state, duration) {
    clearTimeout(state.timeout);
    state.timeout = setTimeout(() => fail(state, 'Code timed out.'), duration);
  }

  function remove(id) {
    const state = states.get(id);
    if (state?.frame) detach(state);
    states.delete(id);
  }

  window.addEventListener('message', ({ source, data }) => {
    const state = frames.get(source);
    if (!state || !data) return;
    if (data.frameReady) {
      source.postMessage({ kind: 'start', source: state.source }, '*');
    } else if (typeof data.error === 'string') {
      fail(state, data.error);
    } else if (data.ready) {
      clearTimeout(state.timeout);
      state.ready = true;
    } else if (state.pending && Array.isArray(data.output) && data.output.length <= 6) {
      clearTimeout(state.timeout);
      state.pending = false;
      state.output = data.output.map(value => Number.isFinite(value)
        ? Math.max(-10, Math.min(10, value)) : 0);
      state.status = running;
    }
  });

  module.cardinalCode = {
    uploadBitmap(gl, bitmap) {
      const premultiplied = gl.getParameter(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL);
      gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);
      try {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, bitmap);
      } finally {
        gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, premultiplied);
        bitmap.close();
      }
    },

    ...installVisualRunner(),
    ...installStrudelRunner(module),
    run(id, source) {
      remove(id);
      const frame = document.createElement('iframe');
      frame.sandbox = 'allow-scripts';
      frame.title = 'Code runner';
      frame.hidden = true;
      frame.srcdoc = runnerDocument;
      document.body.append(frame);
      const state = {
        frame, source, started: performance.now(), lastTick: -Infinity,
        status: starting, output: [0, 0], ready: false, pending: false
      };
      states.set(id, state);
      frames.set(frame.contentWindow, state);
      watchdog(state, 2000);
    },

    exchange(id, voltages, outputCount) {
      const state = states.get(id);
      if (!state) return;
      const now = performance.now();
      if (state.frame && state.ready && !state.pending && now - state.lastTick >= 1000 / 30 - 1) {
        state.lastTick = now;
        state.pending = true;
        const input = voltages.map(value => Number.isFinite(value) ? value : 0);
        state.frame.contentWindow.postMessage({
          kind: 'tick', tick: { input, outputCount, time: (now - state.started) / 1000 }
        }, '*');
        watchdog(state, 500);
      }
      return state;
    },

    takeEditorCommand(id) {
      const commands = editorCommands.get(id);
      if (!commands) return 0;
      const command = commands.shift();
      if (!commands.length) editorCommands.delete(id);
      return command;
    },

    formatEditor(id, glsl, source) {
      const job = {};
      formatJobs.set(id, job);
      formatSource(source, glsl).then(text => {
        job.result = { text: text === source ? '' : text.replace(/\n$/, '') };
      }).catch(error => {
        job.result = { error: 'Format: ' + String(error?.message || error).slice(0, 300) };
      });
    },

    takeFormat(id) {
      const job = formatJobs.get(id);
      if (!job?.result) return null;
      formatJobs.delete(id);
      return job.result;
    },

    discardEditorCommands(id) {
      editorCommands.delete(id);
      formatJobs.delete(id);
    },

    takeSelectAll(id) {
      if (selectAll !== id) return false;
      selectAll = 0;
      return true;
    },

    takePaste(id) {
      if (paste?.editor !== id) return;
      const { text } = paste;
      paste = null;
      return text;
    },

    remove
  };
}
