"""Convert Output Portal presets (XML) to MELTIT VST3 presets built around the Grain card.

The granulator maps onto the Grain card, the two FX slots onto the nearest rack cards, Macro 1 and 2 onto the XY pad's
X and Y, Envelope 1 onto the step sequencer and Envelope 2 onto LFO 1. Everything that has no counterpart is listed in
the report next to the converted presets.
"""

import argparse
from collections import Counter
import json
import math
import os
from pathlib import Path
import re
import sys
from xml.etree import ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))
from import_toneflx import PresetWriter  # noqa: E402

TEMPO = 120.0  # Synced times become milliseconds at this tempo.
FX_SLOTS = ("BitReducer", "Chorus", "DistortionBox", "Filter", "Phaser", "Reverb", "StereoDelay")
MODULES = ("tape", "tremolo", "saturation", "resampler", "chorus", "delay", "reverse", "reverb", "stretch", "vhs", "grain")
MODULE_COUNT = len(MODULES)
PERIOD_MULTIPLIERS = (0, 1 / 8, 1 / 4, 1 / 2, 1, 1.5, 2, 3, 4)
STRETCH_SPEEDS = (0, .125, .25, .33, .375, .5, .625, .66, .75, .875, 1)
PORTAL_PITCH_SPAN = 6.0   # Octave pitch runs from -3 to +3.
OUR_PITCH_SPAN = 48.0     # Grain Pitch runs from -24 to +24 steps.


def division_beats(index):
    index = max(0, min(25, int(round(index))))
    if index >= 18:
        return 4.0 * (index - 17)
    return (1 / 16, 1 / 8, 1 / 4, 1 / 2, 1, 2)[index // 3] * (1 / 1.5, 1, 1.5)[index % 3]


def retrigger_beats(index):
    index = max(0, min(25, int(round(index))))
    if index < 8:
        return 4.0 * (8 - index)
    index -= 8
    return (2, 1, 1 / 2, 1 / 4, 1 / 8, 1 / 16)[index // 3] * (1.5, 1, 1 / 1.5)[index % 3]


def beats_ms(beats):
    return beats * 60000.0 / TEMPO


def clamp(value, low, high):
    return max(low, min(high, value))


# Destination of a modulated Portal parameter: our mod target name, our parameter, its routed range (as the processor's
# route() call has it), how a percent parameter stores it, and how Portal's normalised amount scales onto that range.
TARGETS = {
    ("GranulatorNode", "GrainSize"): ("Grain Size", "grain_size", 10, 1000, 1, 1),
    ("GranulatorNode", "Density"): ("Grain Density", "grain_density", .5, 100, 1, 1),
    ("GranulatorNode", "Pitch"): ("Grain Pitch", "grain_pitch", -24, 24, 1, PORTAL_PITCH_SPAN / OUR_PITCH_SPAN),
    ("GranulatorNode", "PitchRandom"): ("Grain Pitch Random", "grain_pitch_random", 0, 1, 100, PORTAL_PITCH_SPAN / OUR_PITCH_SPAN),
    ("GranulatorNode", "DryWet"): ("Grain Mix", "grain_mix", 0, 1, 100, 1),
    ("GranulatorNode", "DelayTime"): ("Grain Position", "grain_position", 0, 2000, 1, 1),
    ("GranulatorNode", "Feedback"): ("Grain Feedback", "grain_feedback", 0, 1, 100, 1),
    ("GranulatorNode", "WindowShape"): ("Grain Window", "grain_window", 0, 1, 100, 1),
    ("GranulatorNode", "GrainSizeRandom"): ("Grain Size Random", "grain_size_random", 0, 1, 100, 1),
    ("GranulatorNode", "DensityRandom"): ("Grain Density Random", "grain_density_random", 0, 1, 100, 1),
    ("GranulatorNode", "PanRandom"): ("Grain Width", "grain_width", 0, 1, 100, 1),
    ("GranulatorNode", "GrainOffset"): ("Grain Offset", "grain_offset", 0, 250, 1, 1),
    ("GranulatorNode", "NumGrains"): ("Grain Voices", "grain_voices", 1, 16, 1, 1),
    ("GranulatorNode", "StretchSpeed"): ("Grain Stretch Speed", "grain_stretch_speed", 0, 1, 100, 1),
    ("OutputBus", "LowpassCutoff"): ("Grain Tone LPF", "grain_tone_lowpass", 200, 20000, 1, 1),
    ("OutputBus", "HighpassCutoff"): ("Grain Tone HPF", "grain_tone_highpass", 20, 5000, 1, 1),
    ("StereoDelay", "Feedback"): ("Delay Feedback", "delay_feedback", 0, 2, 100, .5),
    ("StereoDelay", "DryWet"): ("Delay Mix", "delay_mix", 0, 1, 100, 1),
    ("Reverb", "DryWet"): ("Reverb Mix", "reverb_mix", 0, 1, 100, 1),
    ("Reverb", "Size"): ("Reverb Size", "reverb_size", 0, 1, 100, 1),
    ("Chorus", "Depth"): ("Chorus Depth", "chorus_depth", 0, 1, 100, 1),
    ("Chorus", "WetDry"): ("Chorus Mix", "chorus_mix", 0, 1, 100, 1),
    ("Chorus", "Rate"): ("Chorus Rate", "chorus_rate", .01, 5, 1, 1),
    ("DistortionBox", "WetDry"): ("Saturation Mix", "saturation_mix", 0, 1, 100, 1),
    ("Filter", "Cutoff"): ("Grain Tone LPF", "grain_tone_lowpass", 200, 20000, 1, 1),
}


def destinations(header):
    text = Path(header).read_text(encoding="utf-8")
    body = text[text.index("static juce::StringArray destinations()"):]
    body = body[body.index("{", body.index("return")) + 1:body.index("};")]
    return re.findall(r'"([^"]+)"', body)


class Preset:
    def __init__(self, root):
        state = root.find("App_State")
        self.values = {p.tag: (float(p.get("mapped_value")), float(p.get("unmapped_value")))
                       for p in state.find("Parameters")}
        self.props = {n.tag: n.attrib for n in state.find("Node_Properties")}
        self.connections = [c.attrib for c in state.find("Connections")]
        self.slots = {s.tag: (int(s.get("ActiveSlot")), s.get("Bypassed") == "1")
                      for s in state.find("Processor_Group_States")}

    def get(self, name, default=0.0):
        return self.values.get(name, (default, default))[0]

    def engine(self, name, default=0.0):
        return self.get("MKUGranulatorNode_MainEngine_" + name, default)

    def mapping(self, name):
        return self.props.get("MKUGranulatorNode_MainEngine", {}).get(name + "CurrentMapping", "0")


def convert(preset, names):
    warnings = set()
    s = {"tape_active": 0, "grain_active": 1, "global_drift": 0, "global_unstable": 0}
    e = preset.engine

    # --- timing: density first, since synced sizes and offsets are multiples of the grain period.
    if preset.mapping("Density") == "1":
        s["grain_sync"] = 1
        s["grain_division"] = int(round(e("Density")))
        period_ms = beats_ms(division_beats(e("Density")))
        s["grain_density"] = clamp(1000.0 / period_ms, .5, 100)
    else:
        seconds = max(e("Density"), 1e-3)
        s["grain_sync"] = 0
        s["grain_density"] = clamp(1.0 / seconds, .5, 100)
        if not .5 <= 1.0 / seconds <= 100:
            warnings.add("Free density was limited to 0.5-100 grains a second.")
        period_ms = 1000.0 / s["grain_density"]

    def period_multiple(name):
        if preset.mapping(name) == "1":
            return PERIOD_MULTIPLIERS[int(clamp(round(e(name)), 0, 8))] * period_ms
        return e(name) * 1000.0

    size = period_multiple("GrainSize")
    s["grain_size"] = clamp(size, 10, 1000)
    if not 10 <= size <= 1000:
        warnings.add(f"Grain size {size:.0f} ms was limited to 10-1000 ms.")
    offset = period_multiple("GrainOffset")
    s["grain_offset"] = clamp(offset, 0, 250)
    position = 0.0
    if e("DelayActive") >= .5:
        position = beats_ms(division_beats(e("DelayTime"))) if preset.mapping("DelayTime") == "1" else e("DelayTime") * 1000.0
    s["grain_position"] = clamp(position, 0, 2000)
    if position > 2000:
        warnings.add("Grain delay was limited to 2 s.")
    if any(preset.mapping(n) == "1" for n in ("DelayTime", "GrainSize", "GrainOffset")):
        warnings.add(f"Synced grain times were fixed at {TEMPO:.0f} BPM.")

    # --- pitch
    scale = int(round(e("PitchScale", 3)))
    if preset.mapping("Pitch") != "3" or scale != 3:
        warnings.add("Pitch mode other than octaves was approximated.")
    s["grain_scale"] = clamp(scale, 0, 16)
    s["grain_pitch"] = clamp(e("Pitch"), -24, 24)
    s["grain_pitch_random"] = clamp(e("PitchRandom") * PORTAL_PITCH_SPAN / OUR_PITCH_SPAN * 100, 0, 100)
    s["grain_reverse"] = 1 if e("ReverseActive") >= .5 else 0

    # --- the rest of the engine
    s["grain_size_random"] = clamp(e("GrainSizeRandom") * 100, 0, 100)
    s["grain_density_random"] = clamp(e("DensityRandom") * 100, 0, 100)
    s["grain_width"] = clamp(e("PanRandom") * 100, 0, 100)
    if abs(e("Pan")) > .05:
        warnings.add("Grain pan offset has no counterpart; grains stay centred.")
    s["grain_window"] = clamp((e("WindowShape", 1) - .02) / .98 * 100, 0, 100)
    s["grain_voices"] = clamp(round(e("NumGrains", 8)), 1, 16)
    s["grain_feedback"] = clamp(e("Feedback") * 100, 0, 100)
    s["grain_lowpass"] = clamp(e("FeedbackLowpassCutoff", 20000), 20, 20000)
    s["grain_highpass"] = clamp(e("FeedbackHighpassCutoff", 20), 20, 2000)
    s["grain_mix"] = clamp(e("DryWet") * 100, 0, 100)
    s["grain_stretch"] = 1 if e("StretchActive") >= .5 else 0
    s["grain_stretch_speed"] = (STRETCH_SPEEDS[int(clamp(round(e("StretchSpeed")), 0, 10))]
                                if preset.mapping("StretchSpeed") == "1" else clamp(e("StretchSpeed"), 0, 1)) * 100
    s["grain_retrigger"] = clamp(round(e("StretchRetriggerRate")), 0, 25) + 1

    # --- output bus: dry/wet, gains and its filters, which land on the Grain card's tone filters.
    out = lambda name, d=0.0: preset.get("MKUOutputBus_MainOutput_" + name, d)  # noqa: E731
    s["global_mix"] = clamp(out("DryWet", 1) * 100, 0, 100)
    s["output_gain"] = clamp(out("OutputGain"), -24, 24)
    s["input_gain"] = clamp(preset.get("MKUInputBus_MainInput_InputGain"), -24, 24)
    tone_low, tone_high = clamp(out("LowpassCutoff", 20000), 200, 20000), clamp(out("HighpassCutoff", 20), 20, 5000)
    if preset.get("MKUCompressor_MasterCompressor_WetDry", 0) > 0:
        warnings.add("Master compressor has no counterpart.")

    # --- FX slots onto rack cards, in their order after the Grain card.
    order = [MODULES.index("grain")]
    used = Counter()
    slot_prefix = {}
    for slot in ("Slot1", "Slot2"):
        index, bypassed = preset.slots.get(slot, (6, True))
        fx = FX_SLOTS[index]
        node = f"MKU{fx}_{slot}"
        p = lambda name, d=0.0: preset.get(f"{node}_{name}", d)  # noqa: E731
        if bypassed:
            continue
        module = {"StereoDelay": "delay", "Reverb": "reverb", "Chorus": "chorus", "DistortionBox": "saturation",
                  "BitReducer": "resampler"}.get(fx)
        if fx == "Filter":
            if p("WetDry", 1) >= .5:
                tone_low = min(tone_low, clamp(p("Cutoff", 20000), 200, 20000))
                warnings.add("Filter slot was taken as a low-pass on the Grain card's tone filter; resonance and drive dropped.")
            slot_prefix[slot] = ("Filter", None)
            continue
        if module is None:
            warnings.add(f"{fx} slot has no counterpart.")
            continue
        instance = MODULES.index(module) + MODULE_COUNT * used[module]
        used[module] += 1
        prefix = ("cart2_" if instance >= MODULE_COUNT else "") + module + "_"
        slot_prefix[slot] = (fx, prefix)
        order.append(instance)
        s[prefix + "active"] = 1
        if fx == "StereoDelay":
            if props_mapping(preset, node, "Time") == "1":
                beats = division_beats(p("Time"))
                index = int(round(p("Time")))
                if index < 18:
                    s[prefix + "sync"] = 1
                    s[prefix + "note"] = index // 3
                    s[prefix + "timing"] = (1, 0, 2)[index % 3]
                else:
                    s[prefix + "sync"] = 0
                    s[prefix + "time"] = clamp(beats_ms(beats), 1, 2500)
                    warnings.add(f"Delay time of {beats:.0f} beats was set in milliseconds at {TEMPO:.0f} BPM.")
            else:
                s[prefix + "sync"] = 0
                s[prefix + "time"] = clamp(p("Time", .375) * 1000, 1, 2500)
            s[prefix + "type"] = 1 if p("Type") >= .5 else 0
            s[prefix + "feedback"] = clamp(p("Feedback") * 100, 0, 100)
            s[prefix + "highpass"] = clamp(p("HighPass", 20), 10, 1000)
            s[prefix + "lowpass"] = clamp(p("LowPass", 20000), 500, 22000)
            s[prefix + "slope"] = 0  # 12 dB: Portal's delay filters are gentler than our 24 dB default.
            s[prefix + "mix"] = clamp(p("DryWet", .5) * 100, 0, 100)
            s[prefix + "saturation"] = 0
            s[prefix + "analog"] = 0
            s[prefix + "mod_depth"] = 0
        elif fx == "Reverb":
            size = clamp(p("Size", .5), 0, 1)
            s[prefix + "type"] = 2
            s[prefix + "size"] = size * 100
            s[prefix + "decay"] = clamp(.6 + 11.4 * size * size, .1, 60)
            s[prefix + "lowpass"] = clamp(20000 * (1 - clamp(p("Damping"), 0, 1)) ** 2, 500, 20000)
            s[prefix + "highpass"] = 10
            s[prefix + "pre_delay"] = 0
            s[prefix + "saturation"] = 0
            s[prefix + "mix"] = clamp(p("DryWet", .3) * 100, 0, 100)
            if p("Freeze") >= .5:
                warnings.add("Reverb freeze has no counterpart.")
            warnings.add("Reverb decay was estimated from Portal's size.")
        elif fx == "Chorus":
            s[prefix + "depth"] = clamp(p("Depth", .5) * 100, 0, 100)
            s[prefix + "rate"] = clamp(p("Rate", .5), .01, 5)
            s[prefix + "width"] = clamp(p("Width") * 200, 0, 100)
            s[prefix + "mix"] = clamp(p("WetDry", .5) * 100, 0, 100)
            s[prefix + "saturation"] = 0
            s[prefix + "stereo"] = 1
            if p("Feedback") > .05:
                warnings.add("Chorus feedback has no counterpart.")
        elif fx == "DistortionBox":
            clip = max(p("ClipGain", .5), 1e-3)
            s[prefix + "mode"] = 1
            s[prefix + "drive"] = clamp(-20 * math.log10(clip), 0, 30.9)
            s[prefix + "mix"] = clamp(p("WetDry", 1) * 100, 0, 100)
            warnings.add("Distortion was approximated by the Saturation card.")
        elif fx == "BitReducer":
            s[prefix + "rate"] = clamp(48000 / max(1, p("SampleReduction", 1)), 44, 44100)
            s[prefix + "drive"] = 0
            warnings.add("Bit Reducer bit depth and mix have no counterpart; only the sample rate was kept.")

    s["grain_tone_lowpass"] = tone_low
    s["grain_tone_highpass"] = tone_high

    # --- modulation: macros onto the XY pad, Envelope 1 onto the sequencer, Envelope 2 onto LFO 1.
    sources = {"MKUMacroNode_Macro1": "xy_x_", "MKUMacroNode_Macro2": "xy_y_",
               "MKUClickableEnvelope_Envelope1": "seq_", "MKUClickableEnvelope_Envelope2": "lfo_1_"}
    routes = {prefix: [] for prefix in sources.values()}
    shifts = Counter()
    skipped = Counter()
    for connection in preset.connections:
        source = connection["SourceNode"]
        if source not in sources:
            continue
        match = re.match(r"(\w+?)Mod(Depth)?In(\d+)$", connection["DestinationChannel"])
        if not match:
            continue
        destination = connection["DestinationNode"]
        if match.group(2):
            skipped["modulation of another modulation's depth"] += 1
            continue
        if source.startswith("MKUClickableEnvelope") and preset.props.get(source, {}).get("Bypassed") == "1":
            continue
        kind = destination.split("_")[0][3:]
        slot = destination.split("_")[-1]
        param = match.group(1)
        node = preset.props.get(destination, {})
        amount = float(node.get(f"{param}ModAmount{match.group(3)}", 0) or 0)
        bipolar = node.get(f"{param}ModBiPolar{match.group(3)}", "0") == "1"
        if amount == 0:
            continue
        if kind in ("StereoDelay", "Reverb", "Chorus", "DistortionBox", "Filter"):
            if slot not in slot_prefix or slot_prefix[slot][0] != kind:
                continue  # A slot that is not playing.
        target = TARGETS.get((kind, param))
        if target is None or (param == "Density" and preset.mapping("Density") == "1") \
                or (param == "DelayTime" and preset.mapping("DelayTime") == "1"):
            skipped[f"{kind} {param}"] += 1
            continue
        name, parameter, low, high, unit, scale = target
        fraction = amount * scale
        routes[sources[source]].append((abs(fraction), name, parameter, low, high, unit, fraction, bipolar))

    for source_prefix, entries in routes.items():
        entries.sort(reverse=True)
        if len(entries) > 4:
            skipped[f"routes past four on {source_prefix.rstrip('_')}"] += len(entries) - 4
        for slot, (_, name, parameter, low, high, unit, fraction, bipolar) in enumerate(entries[:4], 1):
            # Ours swing both ways round the knob: a one-way Portal amount becomes half as deep with the knob moved
            # half way along it, so both cover the same ground.
            depth = fraction * (200 if bipolar else 100)
            if abs(depth) > 100:
                warnings.add(f"{name} modulation was limited to the deepest route.")
            s[f"{source_prefix}target_{slot}"] = names.index(name)
            s[f"{source_prefix}depth_{slot}"] = clamp(depth, -100, 100)
            if source_prefix.startswith("xy_"):
                s[f"{source_prefix}active_{slot}"] = 1
            if not bipolar:
                shifts[parameter] += fraction * .5 * (high - low) * unit
                if parameter not in s:
                    warnings.add(f"{name} was modulated around a value the preset does not set.")
    for parameter, shift in shifts.items():
        if parameter in s:
            low, high = next((t[2] * t[4], t[3] * t[4]) for t in TARGETS.values() if t[1] == parameter)
            s[parameter] = clamp(s[parameter] + shift, low, high)

    # Macro positions go onto the pad; the drawn envelopes are sampled into steps and a sine.
    s["xy_x_amount"] = clamp(preset.get("MKUMacroNode_Macro1_Macro", .5) * 100, 0, 100)
    s["xy_y_amount"] = clamp(preset.get("MKUMacroNode_Macro2_Macro", .5) * 100, 0, 100)
    if routes["seq_"]:
        envelope("Envelope1", preset, s)
        warnings.add("Envelope 1 was sampled into the step sequencer.")
    if routes["lfo_1_"]:
        period = retrigger_beats(preset.get("MKUClickableEnvelope_Envelope2_Rate", 7))
        divisions = (.0625, .125, .25, .5, 1, 2, 4, 8, 16, 32)
        s["lfo_1_active"] = 1
        s["lfo_1_sync"] = 1
        s["lfo_1_shape"] = 4
        s["lfo_1_division"] = min(range(len(divisions)), key=lambda i: abs(math.log(divisions[i] / period)))
        warnings.add("Envelope 2 became a smooth random LFO at the same rate; its drawn shape was lost.")
    for reason, count in skipped.items():
        warnings.add(f"Skipped {count} modulation route(s): {reason}.")

    return s, order, sorted(warnings)


def props_mapping(preset, node, name):
    return preset.props.get(node, {}).get(name + "CurrentMapping", "0")


def envelope(name, preset, s):
    node = preset.props.get("MKUClickableEnvelope_" + name, {})
    count = int(node.get("NumPoints", 0))
    points = sorted((float(node[f"PointX{i}"]), float(node[f"PointY{i}"])) for i in range(count)
                    if f"PointX{i}" in node and f"PointY{i}" in node)
    if len(points) < 2:
        points = [(0, .5), (1, .5)]
    period = retrigger_beats(preset.get(f"MKUClickableEnvelope_{name}_Rate", 7))
    step_beats = (.125, .25, .5, 1, 2, 4)
    division = next((i for i, b in enumerate(step_beats) if period / b <= 16), len(step_beats) - 1)
    length = int(clamp(round(period / step_beats[division]), 1, 16))
    s["seq_division"] = division
    s["seq_length"] = length
    s["seq_glide"] = 60
    for i in range(16):
        x = (i + .5) / length if i < length else 1
        y = points[-1][1]
        for (x0, y0), (x1, y1) in zip(points, points[1:]):
            if x0 <= x <= x1:
                y = y0 + (y1 - y0) * ((x - x0) / (x1 - x0) if x1 > x0 else 0)
                break
        s[f"seq_step_{i + 1}"] = clamp((2 * y - 1) * 100, -100, 100)


def encode(order):
    packed = 1 << 63
    for i, item in enumerate(order):
        packed |= (item + 2) << (5 * i)
    return format(packed, "x")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--plugin", type=Path, required=True, help="MELTIT.vst3 binary with the Grain card")
    parser.add_argument("--limit", type=int, default=0)
    args = parser.parse_args()
    from pedalboard import load_plugin
    import numpy as np

    names = destinations(Path(__file__).resolve().parent.parent / "Source" / "Dsp" / "Modulation.h")
    plugin = load_plugin(str(args.plugin.resolve(strict=True)))
    writer = PresetWriter(plugin.preset_data)
    known = {node.get("id") for node in writer.state.findall("PARAM")}
    missing = {"grain_tone_lowpass", "xy_x_amount", "seq_step_1"} - known
    if missing:
        raise SystemExit(f"The plug-in at {args.plugin} predates the Grain card and XY pad: {sorted(missing)}")
    args.destination.mkdir(parents=True, exist_ok=True)
    jobs = sorted(args.source.glob("*.xml"))
    if args.limit:
        jobs = jobs[:args.limit]

    rate = 48000
    t = np.arange(rate * 4) / rate
    # A pulsing tone over noise, so presets that keep only a narrow band still have something to work with.
    noise = np.random.default_rng(7).standard_normal((2, t.size)) * .05
    tone = .15 * np.sin(2 * np.pi * 220 * t) * (1 + np.sin(2 * np.pi * 1.5 * t)) / 2
    probe = (tone + noise).astype(np.float32)

    report = []
    for path in jobs:
        row = {"source": path.name}
        try:
            settings, order, warnings = convert(Preset(ET.parse(path).getroot()), names)
            unknown = sorted(set(settings) - known)
            if unknown:
                raise ValueError(f"Unknown destination parameters: {unknown}")
            state = writer.state
            state.set("stateVersion", "7")
            state.set("cartridgeOrder", encode(order))
            output = args.destination / (path.stem.title() + ".vstpreset")
            output.write_bytes(writer.render({k: float(v) for k, v in settings.items()}))
            plugin.load_preset(str(output))
            reloaded = {p.get("id"): float(p.get("value")) for p in PresetWriter(plugin.preset_data).state.findall("PARAM")}
            for key in ("grain_active", "grain_size", "grain_mix", "xy_x_amount"):
                # Parameters snap to their step (0.1 for most, 1 for Grain Size).
                if abs(reloaded[key] - settings[key]) > (.51 if key == "grain_size" else .051):
                    raise ValueError(f"{key} did not reload ({reloaded[key]} != {settings[key]})")
            plugin.reset()
            rendered = plugin.process(probe, rate)
            if not np.isfinite(rendered).all():
                raise ValueError("Rendering produced non-finite samples")
            peak = float(np.abs(rendered).max())
            rms = float(np.sqrt(np.mean(rendered[:, rate:] ** 2)))
            row.update(output=output.name, cards=[MODULES[i % MODULE_COUNT] + (" 2" if i >= MODULE_COUNT else "")
                                                   for i in order],
                       peak_db=round(20 * math.log10(max(peak, 1e-9)), 1), rms_db=round(20 * math.log10(max(rms, 1e-9)), 1),
                       notes=warnings)
            if peak > 4 or rms < 1e-4:
                row["check"] = "level looks wrong"
        except (KeyError, OSError, ET.ParseError, RuntimeError, ValueError) as exc:
            row["error"] = str(exc)
        report.append(row)
        print(row.get("output", row.get("error")), row.get("peak_db"), row.get("check", ""))
    notes = Counter(note for row in report for note in row.get("notes", []))
    summary = {"files": len(jobs), "converted": sum("output" in r for r in report),
               "failed": sum("error" in r for r in report), "level_checks": sum("check" in r for r in report),
               "notes": dict(notes.most_common())}
    (args.destination / "import-report.json").write_text(
        json.dumps({"summary": summary, "presets": report}, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    if summary["failed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
