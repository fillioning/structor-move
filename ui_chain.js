/*
 * Structor v0.2.0 — Chain UI
 *
 * Jog wheel menu: navigate and edit all Structor parameters.
 * Knobs 1-8 (CC 71-78): direct parameter control via Shadow UI knob overlay.
 *
 * Knob 7 (Mode) wraps 0-7 across 8 reconstruction modes.
 * Knob 8 is context-sensitive — its label, range, and display format change
 * based on the current mode (Clouds-style dynamic subheader pattern).
 *
 * NOTE: Knobs 1-8 are handled by the DSP via the knob_N_adjust/name/value
 * overlay system. This UI does NOT send set_param for knob CCs — the Shadow
 * host calls the DSP directly. The jog menu can also edit these params via
 * host_module_set_param for the same keys.
 */

import {
    MoveMainKnob,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4,
    MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8,
    LightGrey
} from '/data/UserData/schwung/shared/constants.mjs';

import { decodeDelta, decodeAcceleratedDelta } from '/data/UserData/schwung/shared/input_filter.mjs';

import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter
} from '/data/UserData/schwung/shared/menu_layout.mjs';

const W = 128;
const H = 64;
const CC_JOG_CLICK = 3;

/* ── Mode and enum name tables (must match DSP exactly) ── */

const MODE_NAMES = [
    "Random", "Pitch Up", "Pitch Down", "Density Up",
    "Time Warp", "Dens Arp", "Deltarupt", "Spec Warp"
];

const ARP_PATTERNS = ["Up", "Down", "Up-Down", "Down-Up", "Random", "Cascade"];

const OCTAVE_FOLDS = ["None", "1 Oct", "Mirror", "Harmonic", "Inharm"];

/* Knob 8 label per mode (must match KNOB8_LABELS in structor.c) */
const KNOB8_LABELS = [
    "Shfl Bias", "Pitch Win", "Oct Fold", "Dens Crv",
    "Spd Curve", "Arp Ptrn", "Attack", "Morphing"
];

/* ── Knob 8 per-mode config (range, step, formatter) ── */

const KNOB8_CONFIG = [
    /* 0 Random    */ { min: 0, max: 1,   step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    /* 1 Pitch Up  */ { min: 0, max: 1,   step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    /* 2 Pitch Dn  */ { min: 0, max: 4,   step: 1,    fmt: v => OCTAVE_FOLDS[Math.round(v)] || "?" },
    /* 3 Density   */ { min: 0, max: 1,   step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    /* 4 Time Warp */ { min: 0.5, max: 2, step: 0.01, fmt: v => `${v.toFixed(1)}x` },
    /* 5 Dens Arp  */ { min: 0, max: 5,   step: 1,    fmt: v => ARP_PATTERNS[Math.round(v)] || "?" },
    /* 6 Deltarupt */ { min: 0, max: 1,   step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    /* 7 Spec Warp */ { min: 0, max: 1,   step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` }
];

/* DSP keys for Knob 8 per mode (for jog-menu editing) */
const KNOB8_KEYS = [
    "shuffle_bias", "pitch_range_window", "octave_fold", "density_curve",
    "speed_curve_exp", "arp_pattern", "deltarupt_attack", "density_morphing"
];

/* ── Parameter definitions ── */
/* First 8 are knob-mapped. Knob 8 is a virtual slot resolved at runtime. */

const PARAMS = [
    { key: "detection",  name: "Detection",  min: 0,   max: 1,    step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    { key: "density",    name: "Density",     min: 0.1, max: 1,    step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    { key: "grain_size", name: "Grain Size",  min: 0.5, max: 2,    step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    { key: "time_warp",  name: "Time Warp",   min: 0.5, max: 2,    step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    { key: "feedback",   name: "Feedback",    min: 0,   max: 0.95, step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` },
    { key: "mode",       name: "Mode",        min: 0,   max: 7,    step: 1,    fmt: v => MODE_NAMES[Math.round(v)] || "?" },
    /* Knob 7: context-sensitive Special — resolved dynamically per mode */
    { key: null, name: null, min: 0, max: 1, step: 0.01, fmt: null,
      dynName: () => KNOB8_LABELS[currentMode()] || "Special",
      dynFmt:  (v) => KNOB8_CONFIG[currentMode()].fmt(v),
      dynKey:  () => KNOB8_KEYS[currentMode()]
    },
    { key: "mix",        name: "Mix",         min: 0,   max: 1,    step: 0.01, fmt: v => `${(v * 100).toFixed(0)}%` }
];

const KNOB_CC_BASE = 71;

/* ── State ── */
let paramValues = [0.1, 0.7, 1.0, 1.0, 0.2, 0, 0.5, 0.5];
let selectedParam = 0;
let editMode = false;
let needsRedraw = true;

function currentMode() {
    return Math.round(paramValues[5]);
}

/* ── Param I/O ── */

function getParamKey(index) {
    if (index === 6) return KNOB8_KEYS[currentMode()];
    return PARAMS[index].key;
}

function getParamConfig(index) {
    if (index === 6) {
        const m = currentMode();
        return KNOB8_CONFIG[m];
    }
    return PARAMS[index];
}

function fetchParams() {
    /* Fetch knobs 1-5 + mode (indices 0-5) */
    for (let i = 0; i < 6; i++) {
        const val = host_module_get_param(PARAMS[i].key);
        if (val !== null && val !== undefined && val !== "") {
            const num = parseFloat(val);
            if (num !== paramValues[i]) {
                paramValues[i] = num;
                needsRedraw = true;
            }
        }
    }
    /* Fetch Special value (index 6, mode-dependent key) */
    const spKey = KNOB8_KEYS[currentMode()];
    const spVal = host_module_get_param(spKey);
    if (spVal !== null && spVal !== undefined && spVal !== "") {
        const num = parseFloat(spVal);
        if (num !== paramValues[6]) {
            paramValues[6] = num;
            needsRedraw = true;
        }
    }
    /* Fetch Mix (index 7) */
    const mixVal = host_module_get_param("mix");
    if (mixVal !== null && mixVal !== undefined && mixVal !== "") {
        const num = parseFloat(mixVal);
        if (num !== paramValues[7]) {
            paramValues[7] = num;
            needsRedraw = true;
        }
    }
}

function setParam(index, value) {
    const cfg = getParamConfig(index);
    const key = getParamKey(index);

    /* Integer-step params: round */
    if (cfg.step >= 1) value = Math.round(value);

    /* Mode wraps around 0-7 */
    if (key === "mode") {
        if (value > cfg.max) value = cfg.min;
        if (value < cfg.min) value = cfg.max;
    } else {
        value = Math.max(cfg.min, Math.min(cfg.max, value));
    }

    paramValues[index] = value;

    const valStr = (cfg.step >= 1) ? String(value) : value.toFixed(2);
    host_module_set_param(key, valStr);
    needsRedraw = true;
}

/* ── Display ── */

function fmtValue(index, v) {
    if (index === 6) return KNOB8_CONFIG[currentMode()].fmt(v);
    return PARAMS[index].fmt(v);
}

function paramName(index) {
    if (index === 6) return KNOB8_LABELS[currentMode()] || "Special";
    return PARAMS[index].name;
}

function drawUI() {
    clear_screen();

    /* Header */
    drawHeader("Structor");

    /* Subheader: mode name + Knob 8 context */
    const mode = currentMode();
    const k8Label = KNOB8_LABELS[mode] || "Special";
    const k8Val = fmtValue(6, paramValues[6]);
    const sub = `${MODE_NAMES[mode] || "?"}  ${k8Label} ${k8Val}`;
    print(2, 12, sub, 1);

    /* Parameter list — show 4 visible params around selection */
    const lh = 11;
    const y0 = 24;
    const visible = 4;
    const total = PARAMS.length;
    let startIdx = Math.max(0, Math.min(selectedParam - 1, total - visible));

    for (let vi = 0; vi < visible; vi++) {
        const i = startIdx + vi;
        if (i >= total) break;

        const y = y0 + vi * lh;
        const sel = i === selectedParam;

        if (sel) fill_rect(0, y - 1, W, lh, 1);

        const color = sel ? 0 : 1;
        const prefix = sel ? (editMode ? "* " : "> ") : "  ";
        const name = paramName(i);
        print(2, y, `${prefix}${name}`, color);

        const valStr = fmtValue(i, paramValues[i]);
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

/* ── Lifecycle ── */

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

    if (status !== 0xB0) return;

    /* Jog wheel rotate */
    if (d1 === MoveMainKnob) {
        const delta = decodeDelta(d2);
        if (delta !== 0) {
            if (editMode) {
                setParam(selectedParam, paramValues[selectedParam] + delta * getParamConfig(selectedParam).step);
            } else {
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

    /* Knobs 1-8 are handled by the DSP knob overlay system.
     * The Shadow host intercepts CC 71-78 and calls set_param("knob_N_adjust")
     * directly on the DSP — we do NOT need to handle them here.
     * However, we do need to trigger a redraw when knobs change. */
    const knobIdx = d1 - KNOB_CC_BASE;
    if (knobIdx >= 0 && knobIdx < 8) {
        needsRedraw = true;
        return;
    }
}

globalThis.chain_ui = {
    init,
    tick,
    onMidiMessageInternal
};
