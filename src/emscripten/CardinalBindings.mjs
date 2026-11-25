/* SPDX-License-Identifier: GPL-3.0-or-later */

export function installBindings(module) {
  const invoke = module.cwrap("cardinalWebMCP", "string", ["string", "string"]);
  const send = (operation, args = {}) => JSON.parse(invoke(operation, JSON.stringify(args)));
  const { count } = send("host_parameters");
  if (!count) return;

  const storageKey = "cardinal.host-bindings.v1";
  let bindings = [];
  try {
    const saved = JSON.parse(localStorage.getItem(storageKey) || "[]");
    if (Array.isArray(saved)) {
      bindings = saved.filter(binding => binding && Number.isInteger(binding.slot) &&
        binding.slot >= 1 && binding.slot <= count);
    }
  } catch {}

  const dialog = document.createElement("dialog");
  dialog.className = "cardinal-bindings";
  dialog.setAttribute("aria-labelledby", "cardinal-bindings-title");
  dialog.innerHTML = `
    <header><h1 id="cardinal-bindings-title">Bindings</h1><button type="button" data-close>Close</button></header>
    <p>Choose a Host Param slot, then assign that slot in Host Params Map.</p>
    <div class="bindings-actions">
      <button type="button" data-add>Add binding</button>
      <button type="button" data-midi>Enable MIDI</button>
    </div>
    <p data-status role="status"></p>
    <div class="bindings-list" data-bindings></div>
  `;
  document.body.append(dialog);
  const list = dialog.querySelector("[data-bindings]");
  const status = dialog.querySelector("[data-status]");
  const add = dialog.querySelector("[data-add]");
  let learning = null;
  let midiAccess;
  const midiInputs = new WeakSet();
  const values = new Map();
  const gates = new Map();

  function save() {
    try {
      localStorage.setItem(storageKey, JSON.stringify(bindings));
    } catch {
      status.textContent = "Browser storage is unavailable.";
    }
  }

  function write(slot, value) {
    const result = send("host_parameter", {slot, value});
    if (result.error) {
      status.textContent = result.error;
      return;
    }
    values.set(slot, value);
  }

  function reset(slot) {
    const gate = gates.get(slot);
    if (gate) clearTimeout(gate.timeout);
    gates.delete(slot);
    if (values.get(slot)) write(slot, 0);
  }

  function gateDown(slot) {
    if (gates.has(slot)) return;
    gates.set(slot, { started: performance.now(), timeout: null });
    write(slot, 10);
  }

  function gateUp(slot) {
    const gate = gates.get(slot);
    if (!gate || gate.timeout !== null) return;
    gate.timeout = setTimeout(() => reset(slot), Math.max(0, 120 - (performance.now() - gate.started)));
  }

  function describe(input) {
    if (!input) return "Unassigned";
    if (input.kind === "key") return input.label;
    return `${input.kind === "cc" ? "CC" : "Note"} ${input.number} · Ch ${input.channel + 1}`;
  }

  function learn(input) {
    reset(learning.slot);
    learning.input = input;
    if (input.kind !== "cc" && learning.mode === "continuous") learning.mode = "gate";
    learning = null;
    status.textContent = `Assigned ${describe(input)}.`;
    render();
    save();
  }

  function render() {
    list.replaceChildren();
    add.disabled = bindings.length >= count;
    for (const binding of bindings) {
      const row = document.createElement("div");
      row.className = "binding-row";
      row.innerHTML = `
        <label>Label<input type="text" placeholder="e.g. Sequencer Run"></label>
        <label>Host Param<select data-slot></select></label>
        <label>Mode<select data-mode>
          <option value="gate">Momentary</option><option value="toggle">Toggle</option>
          <option value="continuous">Continuous CC</option>
        </select></label>
        <button type="button" data-learn></button>
        <button type="button" data-test>Test</button>
        <button type="button" data-remove>Remove</button>
      `;
      const label = row.querySelector("input");
      label.value = binding.label || "";
      label.oninput = () => { binding.label = label.value; save(); };
      const slots = row.querySelector("[data-slot]");
      for (let slot = 1; slot <= count; slot++) {
        const option = new Option(`P${String(slot).padStart(2, "0")}`, slot);
        option.disabled = bindings.some(other => other !== binding && other.slot === slot);
        slots.add(option);
      }
      slots.value = binding.slot;
      slots.onchange = () => {
        reset(binding.slot);
        binding.slot = Number(slots.value);
        render();
        save();
      };
      const mode = row.querySelector("[data-mode]");
      mode.value = binding.mode || "gate";
      mode.querySelector('[value="continuous"]').disabled = binding.input?.kind !== "cc";
      mode.onchange = () => {
        reset(binding.slot);
        binding.mode = mode.value;
        save();
      };
      const learnButton = row.querySelector("[data-learn]");
      learnButton.textContent = learning === binding ? "Cancel learn" : `Learn · ${describe(binding.input)}`;
      learnButton.onclick = () => {
        learning = learning === binding ? null : binding;
        status.textContent = learning ? "Press a key or move a MIDI control." : "";
        render();
        if (learning) list.children[bindings.indexOf(binding)].querySelector("[data-learn]").focus();
      };
      row.querySelector("[data-test]").onclick = () => {
        reset(binding.slot);
        gateDown(binding.slot);
        gateUp(binding.slot);
      };
      row.querySelector("[data-remove]").onclick = () => {
        reset(binding.slot);
        if (learning === binding) learning = null;
        bindings = bindings.filter(item => item !== binding);
        status.textContent = "";
        render();
        save();
      };
      list.append(row);
    }
  }

  add.onclick = () => {
    const slot = Array.from({length: count}, (_, index) => index + 1)
      .find(candidate => !bindings.some(binding => binding.slot === candidate));
    if (!slot) return;
    bindings.push({slot, label: "", mode: "gate"});
    render();
    save();
    list.lastElementChild.querySelector("input").focus();
  };
  dialog.querySelector("[data-close]").onclick = () => dialog.close();
  dialog.addEventListener("close", () => {
    learning = null;
    module.canvas.focus();
  });

  const pressed = new Map();
  function keyInput(event) {
    const modifiers = [event.ctrlKey && "Ctrl", event.altKey && "Alt",
      event.shiftKey && "Shift", event.metaKey && "⌘"].filter(Boolean);
    const key = event.key === " " ? "Space" : event.key;
    return {kind: "key", code: event.code, mods: modifiers.join("+"),
      label: [...modifiers, key.length === 1 ? key.toUpperCase() : key].join("+")};
  }

  function consume(event) {
    event.preventDefault();
    event.stopImmediatePropagation();
  }

  window.addEventListener("keydown", event => {
    if (!event.code || /^(Control|Shift|Alt|Meta)(Left|Right)$/.test(event.code)) return;
    if (event.target instanceof Element && event.target.closest("input, select, textarea, [contenteditable]")) return;
    const input = keyInput(event);
    if (learning) {
      consume(event);
      if (event.repeat) return;
      if (event.key === "Escape") {
        learning = null;
        status.textContent = "";
        render();
      } else {
        learn(input);
      }
      return;
    }
    if (dialog.open || module.cardinalCodeEditor || document.activeElement !== module.canvas) return;
    const matches = bindings.filter(binding => binding.input?.kind === "key" &&
      binding.input.code === input.code && binding.input.mods === input.mods);
    if (!matches.length) return;
    consume(event);
    if (event.repeat) return;
    const held = [];
    for (const binding of matches) {
      if (binding.mode === "continuous") continue;
      if (binding.mode === "toggle") write(binding.slot, values.get(binding.slot) ? 0 : 10);
      else {
        gateDown(binding.slot);
        held.push(binding.slot);
      }
    }
    pressed.set(event.code, held);
  }, true);

  window.addEventListener("keyup", event => {
    if (!pressed.has(event.code)) return;
    consume(event);
    for (const slot of pressed.get(event.code)) gateUp(slot);
    pressed.delete(event.code);
  }, true);

  window.addEventListener("blur", () => {
    for (const slot of gates.keys()) reset(slot);
    pressed.clear();
  });

  const midiHeld = new Set();
  function onMidi(event) {
    if (event.data.length < 3) return;
    const [header, number, value] = event.data;
    const command = header & 0xf0;
    const kind = command === 0xb0 ? "cc" : "note";
    if (command !== 0xb0 && command !== 0x90 && command !== 0x80) return;
    const input = {kind, number, channel: header & 0xf};
    const down = kind === "cc" ? value >= 64 : command === 0x90 && value > 0;
    const match = binding => binding.input?.kind === kind &&
      binding.input.number === number && binding.input.channel === input.channel;
    const identity = `${event.currentTarget.id}:${kind}:${input.channel}:${number}`;
    const wasDown = midiHeld.has(identity);
    if (down) midiHeld.add(identity);
    else midiHeld.delete(identity);
    if (learning && (kind === "cc" || down)) {
      learn(input);
      return;
    }
    for (const binding of bindings.filter(match)) {
      if (binding.mode === "continuous") {
        if (kind === "cc") write(binding.slot, 10 * value / 127);
      } else if (binding.mode === "toggle") {
        if (down && !wasDown) write(binding.slot, values.get(binding.slot) ? 0 : 10);
      } else if (down && !wasDown) gateDown(binding.slot);
      else if (!down && wasDown) gateUp(binding.slot);
    }
  }

  function connectMidiInputs() {
    for (const input of midiAccess.inputs.values()) {
      if (midiInputs.has(input)) continue;
      midiInputs.add(input);
      input.addEventListener("midimessage", onMidi);
    }
    for (const slot of gates.keys()) reset(slot);
    midiHeld.clear();
    status.textContent = midiAccess.inputs.size ? "MIDI enabled." : "No MIDI inputs connected.";
  }

  const midiButton = dialog.querySelector("[data-midi]");
  midiButton.onclick = async () => {
    try {
      if (!navigator.requestMIDIAccess) throw new Error("Web MIDI is unavailable in this browser.");
      midiAccess = await navigator.requestMIDIAccess();
      midiAccess.addEventListener("statechange", connectMidiInputs);
      midiButton.disabled = true;
      connectMidiInputs();
    } catch (error) {
      status.textContent = error.message;
    }
  };

  window.cardinalShowBindings = () => {
    if (dialog.open) return;
    status.textContent = "";
    render();
    dialog.showModal();
  };
}
