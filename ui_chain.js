/*
 * Structor — Chain UI
 *
 * Jog wheel menu: navigate and edit all Structor parameters.
 * Knobs 1-8 (CC 71-78): direct parameter control via relative CCs.
 *
 * Knob 8 ("Special") is context-sensitive — its label and value change
 * based on the current mode (Clouds-style dynamic subheader pattern).
 */

import {
    MoveMainKnob,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4,
    MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8,
    LightGrey
} from '/data/UserData/move-anything/shared/constants.mjs';

import { decodeDelta, decodeAcceleratedDelta } from '/data/UserData/move-anything/shared/input_filter.mjs';

import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter
} from '/data/UserData/move-anything/shared/menu_layout.mjs';

const W = 128;
const H = 64;
const CC_JOG_CLICK = 3;

const MODE_NAMES = [
    "Random", "Pitch \u2191", "Pitch \u2193", "Density \u2191",
    "Time Warp", "Density Arp", "Deltarupt"
];

const ARP_PATTERNS = ["Up", "Down", "Up-Down", "Down-Up", "Random", "Cascade"];

/* Knob 8 label per mode */
const KNOB8_LABELS = [
    "Reshuffle", "Scatter", "Scatter", "Curve",
    "Drift", "Arp Ptrn", "Attack"
];

/* Parameter definitions — first 8 are knob-mapped, rest are jog-only */
const PARAMS = [
    { key: "detection",  name: "Detection",  min: 0,   max: 1,    step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    { key: "density",    name: "Density",    min: 0.1, max: 1,    step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    { key: "grain_size", name: "Grain Size", min: 0.5, max: 2,    step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    { key: "time_warp",  name: "Time Warp",  min: 0.5, max: 2,    step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    { key: "mix",        name: "Mix",        min: 0,   max: 1,    step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    { key: "feedback",   name: "Feedback",   min: 0,   max: 0.95, step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    { key: "mode",       name: "Mode",       min: 0,   max: 6,    step: 1,    fmt: v => MODE_NAMES[Math.round(v)] || "?" },
    /* Knob 8: context-sensitive — fmt checks current mode */
    { key: "special",    name: "Special",    min: 0,   max: 1,    step: 0.01,
      fmt: v => {
          const mode = Math.round(paramValues[6]);
          if (mode === 5) return ARP_PATTERNS[Math.round(v * 5)] || "?";
          return `${(v * 100).toFixed(0)}%`;
      },
      dynName: () => KNOB8_LABELS[Math.round(paramValues[6])] || "Special"
    },
    /* Jog-only mode-specific params (indices 8-13) */
    { key: "reshuffle",        name: "Reshuffle", min: 0, max: 1, step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    { key: "scatter",          name: "Scatter",   min: 0, max: 1, step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    { key: "curve",            name: "Curve",     min: 0, max: 1, step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    { key: "drift",            name: "Drift",     min: 0, max: 1, step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    { key: "arp_pattern",      name: "Arp Ptrn",  min: 0, max: 5, step: 1,    fmt: v => ARP_PATTERNS[Math.round(v)] || "?" },
    { key: "deltarupt_attack", name: "Attack",    min: 0, max: 1, step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` }
];

/* Map knob CCs to parameter indices */
const KNOB_CC_BASE = 71;  /* CC 71-78 → params 0-7 */

/* State */
let paramValues = [0.1, 0.7, 1.0, 1.0, 0.5, 0.2, 0, 0.5, 0.5, 0, 0, 0.3, 0, 0.05];
let selectedParam = 0;
let editMode = false;
let needsRedraw = true;

function fetchParams() {
    for (let i = 0; i < PARAMS.length; i++) {
        const val = host_module_get_param(PARAMS[i].key);
        if (val !== null && val !== undefined && val !== "") {
            const num = parseFloat(val);
            if (num !== paramValues[i]) {
                paramValues[i] = num;
                needsRedraw = true;
            }
        }
    }
}

function setParam(index, value) {
    const p = PARAMS[index];
    /* For integer-step params, round */
    if (p.step >= 1) value = Math.round(value);
    /* Wrap mode around 0-6 */
    if (p.key === "mode") {
        if (value > p.max) value = p.min;
        if (value < p.min) value = p.max;
    } else {
        value = Math.max(p.min, Math.min(p.max, value));
    }
    paramValues[index] = value;
    host_module_set_param(p.key, (p.step >= 1) ? String(value) : value.toFixed(2));
    needsRedraw = true;
}

function drawUI() {
    clear_screen();

    /* Header */
    drawHeader("Structor");

    /* Subheader: mode name + Knob 8 context (Clouds pattern) */
    const mode = Math.round(paramValues[6]);
    const k8Label = KNOB8_LABELS[mode] || "Special";
    const k8Val = PARAMS[7].fmt(paramValues[7]);
    const sub = `${MODE_NAMES[mode]}  ${k8Label} ${k8Val}`;
    print(2, 12, sub, 1);

    /* Parameter list — show 4 visible params around selection */
    const lh = 11;
    const y0 = 24;
    const visible = 4;
    let startIdx = Math.max(0, Math.min(selectedParam - 1, PARAMS.length - visible));

    for (let vi = 0; vi < visible; vi++) {
        const i = startIdx + vi;
        if (i >= PARAMS.length) break;

        const y = y0 + vi * lh;
        const sel = i === selectedParam;

        if (sel) fill_rect(0, y - 1, W, lh, 1);

        const color = sel ? 0 : 1;
        const prefix = sel ? (editMode ? "* " : "> ") : "  ";
        /* Use dynamic name for Knob 8 if available */
        const name = (PARAMS[i].dynName) ? PARAMS[i].dynName() : PARAMS[i].name;
        print(2, y, `${prefix}${name}`, color);

        const valStr = PARAMS[i].fmt(paramValues[i]);
        print(W - valStr.length * 6 - 4, y, valStr, color);
    }

    /* Footer */
    if (editMode) {
        drawFooter({ left: "Jog:value", right: "Click:done" });
    } else {
        drawFooter({ left: "Jog:nav", right: "Click:edit" });
    }

    needsRedraw = false;
}

function init() {
    fetchParams();
    needsRedraw = true;
}

function tick() {
    fetchParams();
    if (needsRedraw) drawUI();
}

function onMidiMessageInternal(data) {
    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    /* CC messages */
    if (status === 0xB0) {
        /* Jog wheel rotate */
        if (d1 === MoveMainKnob) {
            const delta = decodeDelta(d2);
            if (delta !== 0) {
                if (editMode) {
                    setParam(selectedParam, paramValues[selectedParam] + delta * PARAMS[selectedParam].step);
                } else {
                    /* Navigate parameter list */
                    selectedParam = Math.max(0, Math.min(PARAMS.length - 1, selectedParam + delta));
                    needsRedraw = true;
                }
            }
            return;
        }

        /* Jog wheel click — toggle edit mode */
        if (d1 === CC_JOG_CLICK && d2 >= 64) {
            editMode = !editMode;
            needsRedraw = true;
            return;
        }

        /* Knobs 1-8 → direct parameter control (first 8 params only) */
        const knobIdx = d1 - KNOB_CC_BASE;
        if (knobIdx >= 0 && knobIdx < 8) {
            const delta = decodeDelta(d2);
            if (delta !== 0) {
                setParam(knobIdx, paramValues[knobIdx] + delta * PARAMS[knobIdx].step);
            }
            return;
        }
    }
}

globalThis.chain_ui = {
    init,
    tick,
    onMidiMessageInternal
};
