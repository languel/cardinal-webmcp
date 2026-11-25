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

    port.onmessage = ({ data: { input, time } }) => {
      try {
        const output = process({ input, time });
        if (!Array.isArray(output) || output.length !== 2)
          throw new Error('Return an array with two output voltages.');
        send({ output: [clamp(output[0]), clamp(output[1])] });
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

export function installCodeRunner(module) {
  // Values match RunStatus in the rack widget.
  const starting = 1;
  const running = 2;
  const failed = 3;
  const states = new Map();
  const frames = new Map();
  let paste;
  let selectAll = 0;

  function focusedEditor() {
    return document.activeElement === module.canvas ? module.cardinalCodeEditor : 0;
  }

  // Leave the browser's paste shortcut intact while skipping the canvas's
  // simulated clipboard. Only the selected rack editor receives the result.
  window.addEventListener('keydown', event => {
    const editor = focusedEditor();
    if (!editor || !(event.ctrlKey || event.metaKey) || event.altKey) return;
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
    } else if (state.pending && Array.isArray(data.output) && data.output.length === 2) {
      clearTimeout(state.timeout);
      state.pending = false;
      state.output = data.output.map(value => Number.isFinite(value)
        ? Math.max(-10, Math.min(10, value)) : 0);
      state.status = running;
    }
  });

  module.cardinalCode = {
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

    exchange(id, a, b) {
      const state = states.get(id);
      if (!state) return;
      const now = performance.now();
      if (state.frame && state.ready && !state.pending && now - state.lastTick >= 1000 / 30 - 1) {
        state.lastTick = now;
        state.pending = true;
        const input = [Number.isFinite(a) ? a : 0, Number.isFinite(b) ? b : 0];
        state.frame.contentWindow.postMessage({
          kind: 'tick', tick: { input, time: (now - state.started) / 1000 }
        }, '*');
        watchdog(state, 500);
      }
      return state;
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
