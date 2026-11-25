"""Convert a local TONEFLX/FFWD library to Tape Multi-FX VST3 presets."""

import argparse
from collections import Counter
from copy import deepcopy
import json
import math
import os
from pathlib import Path
import struct
from xml.etree import ElementTree as ET

from pedalboard import load_plugin


MODULES = ("Tape", "Modulation", "Distortion", "Resampler", "Chorus", "Delay", "ReverseDelay", "Reverb")
BASE_ORDERS = (
    (0, 1, 2), (0, 2, 1), (1, 0, 2), (1, 2, 0), (2, 0, 1), (2, 1, 0),
)
ROUTE_TARGETS = {
    "Tape_Range": 1, "Tape_WowRate": 2, "Modulation_Depth": 3, "Modulation_Rate": 4,
    "Distortion_Drive": 5, "Distortion_Mix": 6, "Resampler_Drive": 7,
    "Resampler_SampleRate": 8, "Chorus_Depth": 9, "Chorus_Saturation": 10,
    "Chorus_Mix": 11, "Delay_Feedback": 12, "Delay_Saturation": 13, "Delay_Mix": 14,
    "ReverseDelay_Feedback": 15, "ReverseDelay_Pitch": 16, "ReverseDelay_Mix": 17,
    "Reverb_Decay": 18, "Reverb_Mix": 19,
}

# Source suffix, destination ID, and conversion of stored physical values.
FIELDS = {
    "Tape": (
        ("Active", "tape_active"), ("Range", "tape_depth", 100), ("Balance", "tape_flutter_amount", 100),
        ("WowRate", "tape_wow_rate"), ("FlutterRate", "tape_flutter_rate"),
        ("DropOutRate", "tape_dropout_rate"), ("DropOutDepth", "tape_dropout_depth", 100),
        ("NoiseActive", "tape_noise"), ("NoiseMode", "tape_noise_type"), ("NoiseGain", "tape_noise_gain"),
    ),
    "Modulation": (
        ("Active", "tremolo_active"), ("Mode", "tremolo_mode"), ("Depth", "tremolo_depth", 100),
        ("Rate", "tremolo_rate"), ("Sync", "tremolo_sync"), ("NoteDivision", "tremolo_note"),
        ("NoteTiming", "tremolo_timing"), ("Shape", "tremolo_shape", 100), ("Stereo", "tremolo_stereo"),
    ),
    "Distortion": (
        ("Active", "saturation_active"), ("Mode", "saturation_mode"), ("Drive", "saturation_drive", "db"),
        ("Frequency", "saturation_frequency"), ("Tone", "saturation_tone", "bipolar"),
        ("Mix", "saturation_mix", 100),
    ),
    "Resampler": (
        ("Active", "resampler_active"), ("Drive", "resampler_drive", "db"), ("SampleRate", "resampler_rate"),
        ("PreFilter", "resampler_pre_filter"), ("PostFilter", "resampler_post_filter"),
        ("Jitter", "resampler_jitter", 100),
    ),
    "Chorus": (
        ("Active", "chorus_active"), ("Depth", "chorus_depth", 100), ("Rate", "chorus_rate"),
        ("Waveform", "chorus_waveform"), ("Saturation", "chorus_saturation"), ("Analog", "chorus_analog", 100),
        ("Stereo", "chorus_stereo"), ("Width", "chorus_width", 100), ("Mix", "chorus_mix", 100),
    ),
    "Delay": (
        ("Active", "delay_active"), ("Type", "delay_type"), ("Adjust", "delay_warp"), ("Time", "delay_time"),
        ("Sync", "delay_sync"), ("NoteDivision", "delay_note"), ("NoteTiming", "delay_timing"),
        ("Feedback", "delay_feedback", 100), ("Freeze", "delay_freeze"), ("Saturation", "delay_saturation"),
        ("Analog", "delay_analog", 100), ("HPF", "delay_highpass"), ("LPF", "delay_lowpass"),
        ("LPFSlope", "delay_slope"), ("ModRate", "delay_mod_rate"), ("ModDepth", "delay_mod_depth", 100),
        ("Mix", "delay_mix", 100),
    ),
    "ReverseDelay": (
        ("Active", "reverse_active"), ("Type", "reverse_type"), ("NoteDivision", "reverse_note"),
        ("NoteTiming", "reverse_timing"), ("Feedback", "reverse_forward", 100), ("HPF", "reverse_highpass"),
        ("LPF", "reverse_lowpass"), ("Pitch", "reverse_pitch"), ("Mix", "reverse_mix", 100),
    ),
    "Reverb": (
        ("Active", "reverb_active"), ("Type", "reverb_type"), ("PreDelay", "reverb_pre_delay"),
        ("Blend", "reverb_early_late", 100), ("Size", "reverb_size", 100), ("Decay", "reverb_decay"),
        ("LowDamp", "reverb_highpass"), ("HighDamp", "reverb_lowpass"), ("ModRate", "reverb_mod_rate"),
        ("ModDepth", "reverb_mod_depth", 100), ("Saturation", "reverb_saturation"), ("Mix", "reverb_mix", 100),
    ),
    "Pitch": (("Semitones", "global_pitch"), ("Cents", "global_detune")),
    "EQ": (
        ("Active", "eq_active"), ("LowFrequency", "eq_low_freq"), ("LowGain", "eq_low_gain"),
        ("MidFrequency", "eq_mid_freq"), ("MidGain", "eq_mid_gain"),
        ("HighFrequency", "eq_high_freq"), ("HighGain", "eq_high_gain"),
    ),
    "Width": (("Width", "global_width", 100),),
}


def number(values, key, default=None):
    if key not in values:
        return default
    value = float(values[key])
    if not math.isfinite(value):
        raise ValueError(f"Non-finite value for {key}")
    return value


def routing(graph):
    ordered = [MODULES.index(module) for module in dict.fromkeys(graph) if module in MODULES]
    ordered.extend(module for module in range(len(MODULES)) if module not in ordered)
    route = {}
    for module in reversed(range(3, len(MODULES))):
        slot_id = ("resampler", "chorus", "delay", "reverse", "reverb")[module - 3]
        route[slot_id + "_slot"] = ordered.index(module)
        ordered.remove(module)
    route["effect_order"] = BASE_ORDERS.index(tuple(ordered))
    return route


def convert(root):
    warnings = set()
    core = root.find("Core0")
    if root.tag in ("SonixAudio", "FFWD") and core is not None:
        source = {p.attrib["id"]: p.attrib["value"] for p in core.findall("Parameters/PARAM") if p.get("value")}
        globals_node = root.find("GlobalParameters")
        globals_ = globals_node.attrib if globals_node is not None else {}
        graph = [m.get("id") for m in core.findall("FXGraph/Module")]
        if not graph:
            warnings.add("No saved routing. Used Tape Multi-FX default order.")
    elif root.tag in {"Core0_" + module for module in MODULES}:
        source, globals_, graph = root.attrib, {}, []
    else:
        raise ValueError(f"Unsupported preset type: {root.tag}")

    # Legacy FFWD repeated the word Tape or Width in a subset of parameter IDs.
    for key, value in list(source.items()):
        if key.startswith("Core0_Tape_Tape"):
            source.setdefault("Core0_Tape_" + key.removeprefix("Core0_Tape_Tape"), value)
        elif key == "Core0_Width_WidthWidth":
            source.setdefault("Core0_Width_Width", value)

    result = {"tape_active": 0, "tape_noise": 1, "delay_time": 375, "saturation_frequency": 500}
    result.update(routing(graph))
    for module, entries in FIELDS.items():
        for field, target, *scale in entries:
            value = number(source, f"Core0_{module}_{field}")
            if value is None:
                continue
            conversion = scale[0] if scale else 1
            if conversion == "db":
                value = 20 * math.log10(max(1, value))
            elif conversion == "bipolar":
                value = (value + 1) * 50
            else:
                value *= conversion
            result[target] = value

        if module in ("Tape", "Modulation", "Distortion", "Resampler", "Delay", "Reverb"):
            target = entries[0][1].removesuffix("_active")
            for old, new in (("Drift", "drift"), ("Instability", "unstable")):
                result[f"{target}_{new}"] = number(source, f"Core0_{module}_{old}", 0) * 100

    if result.get("tape_wow_rate", .6) < .1:
        result["tape_wow_rate"] = .1
        warnings.add("Tape Wow Rate raised to the destination minimum of 0.1 Hz.")
    for key, maximum, label in (("tape_depth", 100, "Tape Depth"), ("resampler_drive", 12, "Resampler Drive")):
        if result.get(key, 0) > maximum:
            result[key] = maximum
            warnings.add(f"{label} reduced to the destination maximum.")
    if result["tape_active"]:
        for field, neutral in (("Flux", 0), ("ScrapeLevel", 0), ("HPF", 20), ("LPF", 22000)):
            if abs(number(source, "Core0_Tape_" + field, neutral) - neutral) > .01:
                warnings.add(f"Tape {field} has no corresponding control.")
    if number(source, "Core0_ReverseDelay_PitchActive", 1) == 0:
        result["reverse_pitch"] = 0
    elif (pitch := result.get("reverse_pitch", 0)) != round(pitch):
        result["reverse_pitch"] = round(pitch)
        if abs(pitch - round(pitch)) > .001:
            warnings.add("Reverse Pitch was rounded to a whole semitone.")
    if (pitch := result.get("global_pitch", 0)) != round(pitch):
        total = pitch + result.get("global_detune", 0) / 100
        semitones = max(-12, min(12, round(total)))
        result["global_pitch"] = semitones
        result["global_detune"] = max(-50, min(50, round((total - semitones) * 100)))
        if abs(total) > 12.5:
            warnings.add("Global Pitch was limited to the destination range.")
    if "Core0_Delay_Sync" not in source and "Core0_Delay_ClockMode" in source:
        result["delay_sync"] = number(source, "Core0_Delay_ClockMode")
    if root.tag == "SonixAudio":
        # TONEFLX stores Analog as Clock, Mod Depth squared and Drift cubed.
        if "delay_analog" not in result and (clock := number(source, "Core0_Delay_Clock")) is not None:
            result["delay_analog"] = clock * 100
        if (depth := number(source, "Core0_Delay_ModDepth")) is not None:
            result["delay_mod_depth"] = math.sqrt(max(0, depth)) * 100
        result["delay_drift"] = math.cbrt(number(source, "Core0_Delay_Drift", 0)) * 100
    if result.get("delay_active") and not result.get("delay_sync", 1) and "Core0_Delay_Time" not in source:
        warnings.add("Legacy Delay Clock could not be converted to milliseconds. Used 375 ms.")
    if number(source, "Core0_Shifter_Active", 0):
        warnings.add("Legacy Shifter has no corresponding module.")
    if number(source, "Core0_Mixer_Active", 0):
        warnings.add("Legacy Mixer sends were omitted.")
    if graph:
        processing_globals = {"Pitch", "EQ", "Width"}
        last_effect = max((i for i, module in enumerate(graph) if module in MODULES), default=-1)
        if any(module in processing_globals for module in graph[:last_effect]):
            warnings.add("Global Pitch, EQ and Width run after the effect modules.")

    for names, target, scale, fallback in (
        (("Global_InputGain", "GlobalInputGain"), "input_gain", 1, 0),
        (("Global_OutputGain", "GlobalOuputGain"), "output_gain", 1, 0),
        (("Global_Mix",), "global_mix", 100, 1),
        (("Global_Drift",), "global_drift", 100, .5),
        (("Global_Instability",), "global_unstable", 100, .5),
    ):
        value = next((globals_[key] for key in names if key in globals_), fallback)
        result[target] = float(value) * scale
    if number(globals_, "Global_Bypass", 0):
        result["global_mix"] = 0

    for i in (1, 2):
        for old, new in (("Active", "active"), ("Rate", "rate"), ("Shape", "shape"),
                         ("Sync", "sync"), ("NoteDivision", "division")):
            value = number(source, f"Core0_MOD{i}_{old}")
            if value is not None:
                result[f"lfo_{i}_{new}"] = value
        prefix = f"Core0_ModMatrix_MOD{i}_Core0_"
        routes = []
        for key in source:
            if key.startswith(prefix) and (depth := number(source, key)):
                target = key.removeprefix(prefix)
                if target in ROUTE_TARGETS:
                    routes.append((target, depth))
                else:
                    warnings.add(f"LFO target {target} has no matching destination.")
        if len(routes) > 4:
            warnings.add(f"LFO {i} was limited to its four strongest supported assignments.")
        for slot, (target, depth) in enumerate(sorted(routes, key=lambda pair: -abs(pair[1]))[:4], 1):
            result[f"lfo_{i}_target_{slot}"] = ROUTE_TARGETS[target]
            result[f"lfo_{i}_depth_{slot}"] = depth * 100

    amount = number(globals_, "Macro0", 0)
    result["macro_amount"] = (amount + 1) * 50
    for i in range(4):
        target = globals_.get(f"Macro0Target{i}", "").removeprefix("Core0_")
        active = number(globals_, f"Macro0TargetActive{i}", 0)
        depth = number(globals_, f"Macro0TargetDepth{i}", 0)
        if target in ROUTE_TARGETS:
            result[f"macro_target_{i + 1}"] = ROUTE_TARGETS[target]
            result[f"macro_active_{i + 1}"] = active
            result[f"macro_depth_{i + 1}"] = depth * 100
        elif target and amount and active and depth:
            warnings.add(f"Macro target {target} has no matching destination.")
    return result, sorted(warnings)


class PresetWriter:
    def __init__(self, template):
        self.header = template[:40]
        list_offset, = struct.unpack_from("<Q", template, 40)
        if self.header[:4] != b"VST3" or template[list_offset:list_offset + 4] != b"List":
            raise ValueError("Tape Multi-FX did not provide a VST3 preset template")
        count, = struct.unpack_from("<I", template, list_offset + 4)
        self.chunks = {}
        for index in range(count):
            name, offset, length = struct.unpack_from("<4sQQ", template, list_offset + 8 + 20 * index)
            self.chunks[name] = template[offset:offset + length]
        component = self.chunks[b"Comp"]
        length, = struct.unpack_from("<I", component, 4)
        self.state = ET.fromstring(component[8:8 + length])
        self.trailer = component[8 + length:]
        if component[:4] != b"VC2!" or self.state.tag != "TapeMultiFxSettings":
            raise ValueError("The destination plug-in is not Tape Multi-FX")

    def render(self, settings):
        state = deepcopy(self.state)
        nodes = {node.get("id"): node for node in state.findall("PARAM")}
        for key, value in settings.items():
            if not math.isfinite(value):
                raise ValueError(f"Non-finite destination value for {key}")
            nodes[key].set("value", format(value, ".10g"))
        xml = ET.tostring(state, encoding="utf-8", xml_declaration=True)
        chunks = self.chunks | {b"Comp": b"VC2!" + struct.pack("<I", len(xml)) + xml + self.trailer}
        # JUCE's private trailer is kept byte-for-byte and follows the public component state.
        payload = bytearray()
        directory = bytearray(struct.pack("<4sI", b"List", len(chunks)))
        for name, data in chunks.items():
            directory.extend(struct.pack("<4sQQ", name, 48 + len(payload), len(data)))
            payload.extend(data)
        return self.header + struct.pack("<Q", 48 + len(payload)) + payload + directory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--plugin", type=Path, default=Path(os.environ["CommonProgramFiles"]) / "VST3" /
                        "Tape Multi-FX.vst3" / "Contents" / "x86_64-win" / "Tape Multi-FX.vst3")
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    destination = args.destination.resolve()
    if not source.is_dir() or destination.is_relative_to(source) or source.is_relative_to(destination):
        parser.error("Source and destination must be separate directories.")
    plugin = load_plugin(str(args.plugin.resolve(strict=True)))
    writer = PresetWriter(plugin.preset_data)
    destination.mkdir(parents=True, exist_ok=False)
    jobs = []
    for path in sorted(source.rglob("*")):
        if path.is_file() and path.suffix.lower() in {".fwd", ".fmod"}:
            jobs.append(path)
    stats = Counter()
    report = []
    used = set()
    for path in jobs:
        relative = path.relative_to(source)
        row = {"source": relative.as_posix()}
        try:
            root = ET.parse(path).getroot()
            settings, warnings = convert(root)
            output = relative.with_suffix(".vstpreset")
            if str(output).casefold() in used:
                output = relative.with_name(relative.stem + " (" + path.suffix[1:] + ").vstpreset")
            if str(output).casefold() in used:
                raise ValueError("Duplicate destination name")
            converted = destination / output
            converted.parent.mkdir(parents=True, exist_ok=True)
            converted.write_bytes(writer.render(settings))
            plugin.load_preset(str(converted))
            reloaded = PresetWriter(plugin.preset_data).state
            reloaded_values = {p.get("id"): float(p.get("value")) for p in reloaded.findall("PARAM")}
            for key in ("effect_order", "resampler_slot", "chorus_slot", "delay_slot", "reverse_slot", "reverb_slot"):
                if reloaded_values[key] != settings[key]:
                    raise ValueError(f"Routing failed to reload for {key}")
            for key in ("tape_active", "tremolo_active", "saturation_active", "resampler_active",
                        "chorus_active", "delay_active", "reverse_active", "reverb_active"):
                if reloaded_values[key] != round(settings.get(key, 0)):
                    raise ValueError(f"Module state failed to reload for {key}")
            used.add(str(output).casefold())
            stats[root.tag] += 1
            row.update(output=output.as_posix(), file_type=root.tag, warnings=warnings)
        except (KeyError, OSError, ET.ParseError, RuntimeError, ValueError) as exc:
            row["error"] = str(exc)
        report.append(row)
    errors = sum("error" in row for row in report)
    partial = sum(bool(row.get("warnings")) for row in report)
    summary = {"files": len(jobs), "verified": sum(stats.values()), "with_conversion_notes": partial,
               "failed": errors, "types": dict(stats)}
    (destination / "import-report.json").write_text(
        json.dumps({"summary": summary, "presets": report}, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False))
    if errors or not jobs:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
