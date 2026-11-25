#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace rack
{
struct PresetValue
{
    const char* id;
    float value, variation = 0;
    bool operator==(const PresetValue&) const = default;
};

struct Preset
{
    const char* name;
    int tag;
    std::vector<PresetValue> values;
};

inline constexpr std::array tagNames { "Any", "Texture", "Motion", "Echo", "Space" };
inline constexpr int generatedOriginBase = 100;

struct PresetIdentity
{
    int origin = -1;
    int variation = -1;
    bool operator==(const PresetIdentity&) const = default;
};

struct GeneratedPreset
{
    std::vector<PresetValue> values;
    int variation = -1;
    bool operator==(const GeneratedPreset&) const = default;
};

inline const std::array<Preset, 9>& presets()
{
    static const std::array<Preset, 9> library {{
        { "Initialize", 0, {{ "tape_active", 0 }} },
        { "Line Print", 1, {{ "tape_depth", 20, 8 }, { "tape_aged_color", 23, 8 }, { "tape_drift", 17, 6 },
            { "saturation_active", 1 }, { "saturation_mode", 0 }, { "saturation_drive", 4.7f, 2 },
            { "saturation_mix", 34, 7 }, { "output_gain", -2 }} },
        { "Low-Band Copy", 1, {{ "tape_depth", 15, 6 }, { "resampler_active", 1 }, { "resampler_rate", 11800, 2400 },
            { "resampler_slot", 0 }, { "resampler_jitter", 5, 3 }, { "resampler_drive", 1.7f, 1 },
            { "eq_high_gain", -2.6f, 1 }, { "global_mix", 65, 10 }} },
        { "Wide Reel", 2, {{ "tape_depth", 18, 6 }, { "tape_flutter_amount", 25, 8 }, { "chorus_active", 1 },
            { "chorus_depth", 34, 10 }, { "chorus_rate", .3f, .12f }, { "chorus_mix", 26, 8 },
            { "chorus_width", 54, 9 }, { "global_width", 109, 10 }} },
        { "Pulse Send", 2, {{ "tape_active", 0 }, { "tremolo_active", 1 }, { "tremolo_note", 4 },
            { "tremolo_depth", 57, 12 }, { "tremolo_stereo", 1 }, { "tremolo_shape", 39, 9 },
            { "chorus_active", 1 }, { "chorus_mix", 16, 6 }, { "global_mix", 74, 10 }} },
        { "Tape Repeat", 3, {{ "tape_depth", 17, 7 }, { "delay_active", 1 }, { "delay_type", 1 },
            { "delay_feedback", 37, 8 }, { "delay_lowpass", 3900, 1100 }, { "delay_highpass", 140, 60 },
            { "delay_mix", 26, 7 }, { "delay_drift", 16, 7 }} },
        { "Reverse Splice", 3, {{ "tape_active", 0 }, { "reverse_active", 1 }, { "reverse_note", 4 },
            { "reverse_forward", 23, 8 }, { "reverse_mix", 39, 9 }, { "delay_active", 1 },
            { "delay_note", 3 }, { "delay_feedback", 26, 8 }, { "delay_mix", 16, 6 }, { "reverse_slot", 3 }} },
        { "Room Return", 4, {{ "tape_active", 0 }, { "reverb_active", 1 }, { "reverb_type", 0 },
            { "reverb_decay", .86f, .3f }, { "reverb_size", 34, 8 }, { "reverb_mix", 23, 6 },
            { "reverb_highpass", 135, 40 }, { "reverb_lowpass", 6400, 1000 }} },
        { "Hall Return", 4, {{ "tape_depth", 16, 6 }, { "chorus_active", 1 }, { "chorus_mix", 17, 6 },
            { "reverb_active", 1 }, { "reverb_type", 2 }, { "reverb_decay", 4.8f, 1.1f },
            { "reverb_pre_delay", 31, 9 }, { "reverb_mix", 30, 7 }, { "reverb_lowpass", 5300, 1000 }} }
    }};
    return library;
}

// The cartridge a preset value belongs to ("delay" for "delay_mix"), or empty for rack-wide values
// (global mix, width, output, the master EQ).
struct CartridgeIds
{
    const char* name;
    const char* active;
};

inline constexpr std::array<CartridgeIds, 11> cartridgeIds {{
    { "tape", "tape_active" }, { "tremolo", "tremolo_active" }, { "saturation", "saturation_active" },
    { "resampler", "resampler_active" }, { "chorus", "chorus_active" }, { "delay", "delay_active" },
    { "reverse", "reverse_active" }, { "reverb", "reverb_active" }, { "stretch", "stretch_active" },
    { "vhs", "vhs_active" }, { "grain", "grain_active" } }};

inline std::string cartridgeOf(const char* id)
{
    const std::string name(id);
    for (const auto& cartridge : cartridgeIds)
        if (name.rfind(std::string(cartridge.name) + "_", 0) == 0)
            return cartridge.name;
    return {};
}

inline const char* activeIdOf(const std::string& cartridge)
{
    for (const auto& ids : cartridgeIds)
        if (cartridge == ids.name)
            return ids.active;
    return nullptr;
}

// A generated rack's name: two words picked from the variation number, so the same variation always reads the same.
inline std::string generatedName(int variation)
{
    static constexpr std::array adjectives { "Amber", "Velvet", "Dusty", "Hollow", "Faded", "Molten", "Silver",
        "Warm", "Broken", "Drifting", "Lazy", "Copper", "Midnight", "Pale", "Wobbly", "Smoky", "Rusty", "Golden",
        "Soft", "Grainy", "Distant", "Sunlit", "Murky", "Glass", "Cobalt", "Ghost", "Worn", "Crooked", "Sleepy",
        "Bent", "Paper", "Neon" };
    static constexpr std::array nouns { "Reel", "Loop", "Echo", "Splice", "Drift", "Signal", "Bloom", "Haze",
        "Dub", "Ribbon", "Spool", "Memory", "Room", "Tide", "Pulse", "Fade", "Wash", "Dream", "Garden", "Engine",
        "Hum", "Shimmer", "Capstan", "Hiss", "Halo", "Mirror", "Postcard", "Lantern", "Static", "Motel", "Orbit",
        "Choir" };
    std::mt19937 random(static_cast<uint32_t>(variation));
    const auto* adjective = adjectives[random() % adjectives.size()];
    const auto* noun = nouns[random() % nouns.size()];
    return std::string(adjective) + " " + noun;
}

// Builds a new rack from cartridges set up in different presets: a lead preset (of the chosen style) gives one of
// its cartridges and the rack-wide settings, and two or three more cartridges come from other presets, each with
// its own preset's settings, nudged a little.
inline GeneratedPreset generatePreset(int tag, uint32_t seed)
{
    struct Part
    {
        size_t preset;
        std::string cartridge;
    };
    // Every cartridge a preset switches on, with the preset it is set up in.
    std::vector<Part> parts;
    for (size_t i = 1; i < presets().size(); ++i)
    {
        std::vector<std::string> seen;
        for (const auto& entry : presets()[i].values)
        {
            const auto cartridge = cartridgeOf(entry.id);
            if (cartridge.empty() || std::find(seen.begin(), seen.end(), cartridge) != seen.end())
                continue;
            seen.push_back(cartridge);
            const auto active = cartridge + "_active";
            const auto& values = presets()[i].values;
            const auto off = std::any_of(values.begin(), values.end(), [&active](const auto& value)
                { return active == value.id && value.value < .5f; });
            if (!off)
                parts.push_back({ i, cartridge });
        }
    }

    std::mt19937 random(seed);
    std::vector<size_t> leads;
    for (size_t i = 0; i < parts.size(); ++i)
        if (tag == 0 || presets()[parts[i].preset].tag == tag)
            leads.push_back(i);
    if (leads.empty())
        return {};

    std::vector<Part> chosen { parts[leads[random() % leads.size()]] };
    std::vector<size_t> order(parts.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::shuffle(order.begin(), order.end(), random);
    const auto wanted = 3 + random() % 2;
    // Prefer cartridges from presets not used yet; fall back to any preset for a cartridge not in the rack.
    for (const bool freshPreset : { true, false })
        for (const auto i : order)
        {
            if (chosen.size() >= wanted)
                break;
            const auto& part = parts[i];
            const auto sameCartridge = std::any_of(chosen.begin(), chosen.end(),
                [&part](const auto& other) { return other.cartridge == part.cartridge; });
            const auto samePreset = std::any_of(chosen.begin(), chosen.end(),
                [&part](const auto& other) { return other.preset == part.preset; });
            if (!sameCartridge && (!freshPreset || !samePreset))
                chosen.push_back(part);
        }

    const auto nudge = [&random](PresetValue entry)
    {
        if (entry.variation != 0)
        {
            const auto bipolar = (static_cast<float>(random() & 0xffffu) / 65535.0f) * 2.0f - 1.0f;
            entry.value += .8f * entry.variation * bipolar;
        }
        return entry;
    };
    std::vector<PresetValue> values;
    for (const auto& entry : presets()[chosen.front().preset].values)
        if (cartridgeOf(entry.id).empty())
            values.push_back(nudge(entry));
    bool tape = false;
    for (const auto& part : chosen)
    {
        tape = tape || part.cartridge == "tape";
        for (const auto& entry : presets()[part.preset].values)
            if (cartridgeOf(entry.id) == part.cartridge && part.cartridge + "_active" != entry.id)
                values.push_back(nudge(entry));
    }
    // Every effect starts switched off except tape, so name the ones this rack uses and silence tape if it isn't.
    for (const auto& part : chosen)
        values.push_back({ activeIdOf(part.cartridge), 1 });
    if (!tape)
        values.push_back({ "tape_active", 0 });
    return { std::move(values), static_cast<int>(seed & 0x7fffffffu) };
}
}
