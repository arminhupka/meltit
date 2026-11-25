#pragma once

#include "Effects.h"

#include <cstdint>
#include <optional>

namespace rack
{
enum ModTarget
{
    noTarget, tapeDepthTarget, tapeWowTarget, tremoloDepthTarget, tremoloRateTarget,
    saturationDriveTarget, saturationMixTarget, resamplerDriveTarget, resamplerRateTarget,
    chorusDepthTarget, chorusSaturationTarget, chorusMixTarget, delayFeedbackTarget,
    delaySaturationTarget, delayMixTarget, reverseForwardTarget, reversePitchTarget, reverseMixTarget,
    reverbDecayTarget, reverbMixTarget, stretchRatioTarget, stretchCycleTarget, stretchMixTarget,
    vhsWearTarget, vhsTrackingTarget, vhsWobbleTarget, vhsMixTarget,
    // Every other knob on the cards, added later; new targets always go at the end so saved routes keep pointing
    // at the same knobs.
    tapeFlutterRateTarget, tapeFlutterAmountTarget, tremoloShapeTarget, saturationFrequencyTarget,
    saturationToneTarget, resamplerJitterTarget, chorusRateTarget, chorusWidthTarget, delayAnalogTarget,
    reverseHighpassTarget, reverbSizeTarget, reverbPreDelayTarget,
    grainSizeTarget, grainDensityTarget, grainPitchTarget, grainMixTarget,
    grainPositionTarget, grainFeedbackTarget, grainWindowTarget, grainPitchRandomTarget, grainSizeRandomTarget,
    grainDensityRandomTarget, grainWidthTarget, grainOffsetTarget, grainVoicesTarget, grainStretchSpeedTarget,
    grainToneLowpassTarget, grainToneHighpassTarget, modTargetCount
};
using ModOffsets = std::array<float, modTargetCount>;

enum MotionPedal { motionTape, motionTremolo, motionSaturation, motionResampler, motionDelay, motionReverb, motionPedalCount };
struct MotionAmount { float drift = 0, unstable = 0; };
using MotionSettings = std::array<MotionAmount, motionPedalCount>;

struct Route { int target = noTarget; float depth = 0; };
struct LfoSettings
{
    float rate = .5f;
    int shape = 0, division = 5;
    bool active = true, sync = true;
    std::array<Route, 4> routes;
};
// Envelope follower: follows the level of the plugin's input or its sidechain.
struct EnvSettings
{
    float attack = 10, release = 150, gain = 6; // ms, ms, dB
    bool sidechain = false;
    std::array<Route, 4> routes;
};

// Step sequencer: up to sixteen steps from -1 to 1, synced to the tempo.
struct SeqSettings
{
    int division = 1, length = 16;
    float glide = .1f, swing = 0;
    std::array<float, 16> steps {};
    std::array<Route, 4> routes;
};

struct ModulationSettings
{
    std::array<LfoSettings, 2> lfos;
    std::array<Route, 4> macroRoutes;
    float macro = 0;
    // The XY pad: two more macros, moved together by dragging on the pad.
    std::array<Route, 4> xRoutes, yRoutes;
    float x = 0, y = 0;
    EnvSettings env;
    SeqSettings seq;
};

// What the sources are doing now, for the lamps and screens in the editor.
struct ModulationActivity
{
    float lfo1 = 0, lfo2 = 0, macro = 0, envelope = 0, sequence = 0, x = 0, y = 0;
    int step = 0;
};

class ModulationParameters
{
    using Value = std::atomic<float>;
    static constexpr std::array<const char*, motionPedalCount> motionNames
        { "Tape", "Tremolo", "Saturation", "Resampler", "Delay", "Reverb" };
    static constexpr std::array<const char*, motionPedalCount> motionIds
        { "tape", "tremolo", "saturation", "resampler", "delay", "reverb" };

public:
    explicit ModulationParameters(juce::AudioProcessorValueTreeState& state)
    {
        for (size_t i = 0; i < lfoParameters.size(); ++i)
        {
            const auto prefix = lfoId(i);
            auto& lfo = lfoParameters[i];
            lfo.active = state.getRawParameterValue(prefix + "active");
            lfo.shape = state.getRawParameterValue(prefix + "shape");
            lfo.rate = state.getRawParameterValue(prefix + "rate");
            lfo.sync = state.getRawParameterValue(prefix + "sync");
            lfo.division = state.getRawParameterValue(prefix + "division");
            bindRoutes(state, prefix, lfo.routes, false);
        }
        macro = state.getRawParameterValue("macro_amount");
        bindRoutes(state, "macro_", macroRoutes, true);
        padX = state.getRawParameterValue("xy_x_amount");
        padY = state.getRawParameterValue("xy_y_amount");
        bindRoutes(state, "xy_x_", xRoutes, true);
        bindRoutes(state, "xy_y_", yRoutes, true);
        env = { state.getRawParameterValue("env_source"), state.getRawParameterValue("env_attack"),
                state.getRawParameterValue("env_release"), state.getRawParameterValue("env_gain") };
        bindRoutes(state, "env_", envRoutes, false);
        seq.division = state.getRawParameterValue("seq_division");
        seq.length = state.getRawParameterValue("seq_length");
        seq.glide = state.getRawParameterValue("seq_glide");
        seq.swing = state.getRawParameterValue("seq_swing");
        for (size_t i = 0; i < seq.steps.size(); ++i)
            seq.steps[i] = state.getRawParameterValue("seq_step_" + slotId(i));
        bindRoutes(state, "seq_", seqRoutes, false);
        globalMotion = bindMotion(state, "global");
        for (size_t i = 0; i < localMotion.size(); ++i)
        {
            localMotion[i] = bindMotion(state, motionIds[i]);
            secondaryMotion[i] = bindMotion(state, "cart2_" + juce::String(motionIds[i]));
        }
    }

    static void add(juce::AudioProcessorValueTreeState::ParameterLayout& layout)
    {
        const auto toggle = [&layout](const juce::String& id, const juce::String& name)
        {
            layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID { id, 1 }, name, true));
        };
        const auto choice = [&layout](const juce::String& id, const juce::String& name,
                                      juce::StringArray choices, int defaultIndex)
        {
            layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID { id, 1 }, name,
                                                                  std::move(choices), defaultIndex));
        };
        const auto number = [&layout](const juce::String& id, const juce::String& name, juce::NormalisableRange<float> range,
                                      float defaultValue, const char* unit = "%")
        {
            auto attributes = juce::AudioParameterFloatAttributes().withLabel(unit);
            // Percentages and milliseconds read as whole numbers everywhere (knobs, readouts, host).
            if (juce::String(unit) == "%" || juce::String(unit) == "ms")
                attributes = attributes.withStringFromValueFunction([](float v, int) { return juce::String(juce::roundToInt(v)); });
            layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { id, 1 }, name, range,
                defaultValue, attributes));
        };
        const auto addRoutes = [&](const juce::String& prefix, const juce::String& name, bool perRouteToggle)
        {
            for (size_t slot = 0; slot < 4; ++slot)
            {
                const auto suffix = slotId(slot);
                if (perRouteToggle)
                    toggle(prefix + "active_" + suffix, name + "Active " + suffix);
                choice(prefix + "target_" + suffix, name + "Target " + suffix, destinations(), noTarget);
                number(prefix + "depth_" + suffix, name + "Depth " + suffix, { -100, 100, .1f }, 0);
            }
        };
        const auto addMotion = [&](const juce::String& id, const juce::String& name, float defaultValue)
        {
            number(id + "_drift", name + " Drift", { 0, 100, .1f }, defaultValue);
            number(id + "_unstable", name + " Unstable", { 0, 100, .1f }, defaultValue);
        };

        addMotion("global", "Global", 50);
        for (size_t i = 0; i < motionNames.size(); ++i)
        {
            addMotion(motionIds[i], motionNames[i], 0);
            addMotion("cart2_" + juce::String(motionIds[i]), juce::String(motionNames[i]) + " 2", 0);
        }

        for (size_t i = 0; i < 2; ++i)
        {
            const auto prefix = lfoId(i);
            const auto name = "LFO " + slotId(i) + " ";
            toggle(prefix + "active", name + "Active");
            choice(prefix + "shape", name + "Shape", { "Sine", "Triangle", "Square", "Random 1", "Random 2", "Random 3" }, 0);
            juce::NormalisableRange<float> rate { .1f, 20, .001f };
            rate.setSkewForCentre(2.59f);
            number(prefix + "rate", name + "Rate", rate, .5f, "Hz");
            toggle(prefix + "sync", name + "Sync");
            choice(prefix + "division", name + "Division", { "1/64", "1/32", "1/16", "1/8", "1/4", "1/2",
                    "1 Bar", "2 Bars", "4 Bars", "8 Bars" }, 5);
            addRoutes(prefix, name, false);
        }
        number("macro_amount", "Macro Amount", { 0, 100, .1f }, 50);
        addRoutes("macro_", "Macro ", true);
        // The pad starts in the middle, where it changes nothing.
        number("xy_x_amount", "XY X", { 0, 100, .1f }, 50);
        addRoutes("xy_x_", "XY X ", true);
        number("xy_y_amount", "XY Y", { 0, 100, .1f }, 50);
        addRoutes("xy_y_", "XY Y ", true);

        choice("env_source", "ENV Source", { "Input", "Sidechain" }, 0);
        juce::NormalisableRange<float> attack { .1f, 500, .1f };
        attack.setSkewForCentre(30);
        number("env_attack", "ENV Attack", attack, 10, "ms");
        juce::NormalisableRange<float> release { 5, 2000, 1 };
        release.setSkewForCentre(200);
        number("env_release", "ENV Release", release, 150, "ms");
        number("env_gain", "ENV Gain", { -12, 36, .1f }, 6, "dB");
        addRoutes("env_", "ENV ", false);

        choice("seq_division", "SEQ Division", { "1/32", "1/16", "1/8", "1/4", "1/2", "1 Bar" }, 1);
        number("seq_length", "SEQ Length", { 1, 16, 1 }, 16, "steps");
        number("seq_glide", "SEQ Glide", { 0, 100, .1f }, 10);
        number("seq_swing", "SEQ Swing", { 0, 75, .1f }, 0);
        for (size_t i = 0; i < 16; ++i)
            number("seq_step_" + slotId(i), "SEQ Step " + slotId(i), { -100, 100, .1f }, 0);
        addRoutes("seq_", "SEQ ", false);
    }

    ModulationSettings settings() const
    {
        ModulationSettings output;
        for (size_t i = 0; i < lfoParameters.size(); ++i)
        {
            const auto& lfo = lfoParameters[i];
            output.lfos[i] = { raw(lfo.rate), bounded(lfo.shape, 5), bounded(lfo.division, 9),
                               raw(lfo.active) > .5f, raw(lfo.sync) > .5f, readRoutes(lfo.routes) };
        }
        output.macro = raw(macro) * .02f - 1;
        output.macroRoutes = readRoutes(macroRoutes);
        output.x = raw(padX) * .02f - 1;
        output.y = raw(padY) * .02f - 1;
        output.xRoutes = readRoutes(xRoutes);
        output.yRoutes = readRoutes(yRoutes);
        output.env = { raw(env.attack), raw(env.release), raw(env.gain), raw(env.source) > .5f, readRoutes(envRoutes) };
        output.seq.division = bounded(seq.division, 5);
        output.seq.length = juce::jlimit(1, 16, static_cast<int>(std::lround(raw(seq.length))));
        output.seq.glide = raw(seq.glide) * .01f;
        output.seq.swing = raw(seq.swing) * .01f;
        for (size_t i = 0; i < seq.steps.size(); ++i)
            output.seq.steps[i] = raw(seq.steps[i]) * .01f;
        output.seq.routes = readRoutes(seqRoutes);
        return output;
    }

    MotionSettings motion(int layer = 0) const
    {
        MotionSettings result;
        const auto globalDrift = raw(globalMotion.drift) * .0002f;
        const auto globalUnstable = raw(globalMotion.unstable) * .0002f;
        const auto& bank = layer == 0 ? localMotion : secondaryMotion;
        for (size_t i = 0; i < bank.size(); ++i)
            result[i] = { raw(bank[i].drift) * globalDrift, raw(bank[i].unstable) * globalUnstable };
        return result;
    }

private:
    struct MotionValues { const Value* drift; const Value* unstable; };
    struct RouteValues { const Value* target; const Value* depth; const Value* active; };
    struct LfoValues
    {
        const Value* active;
        const Value* shape;
        const Value* rate;
        const Value* sync;
        const Value* division;
        std::array<RouteValues, 4> routes;
    };

    static juce::String slotId(size_t index) { return juce::String(static_cast<int>(index + 1)); }
    static juce::String lfoId(size_t index) { return "lfo_" + slotId(index) + "_"; }
    static juce::StringArray destinations()
    {
        return { "None", "Tape Depth", "Tape Wow Rate", "Tremolo Depth", "Tremolo Rate", "Saturation Drive",
                 "Saturation Mix", "Resampler Drive", "Resampler Rate", "Chorus Depth", "Chorus Saturation", "Chorus Mix",
                 "Delay Feedback", "Delay Saturation", "Delay Mix", "Reverse Forward", "Reverse Pitch", "Reverse Mix",
                 "Reverb Decay", "Reverb Mix", "Stretch Ratio", "Stretch Cycle", "Stretch Mix",
                 "VHS Wear", "VHS Tracking", "VHS Wobble", "VHS Mix",
                 "Tape Flutter Rate", "Tape Flutter Amount", "Tremolo Shape", "Saturation Frequency",
                 "Saturation Tone", "Resampler Jitter", "Chorus Rate", "Chorus Width", "Delay Analog",
                 "Reverse HPF", "Reverb Size", "Reverb Pre Delay",
                 "Grain Size", "Grain Density", "Grain Pitch", "Grain Mix",
                 "Grain Position", "Grain Feedback", "Grain Window", "Grain Pitch Random", "Grain Size Random",
                 "Grain Density Random", "Grain Width", "Grain Offset", "Grain Voices", "Grain Stretch Speed",
                 "Grain Tone LPF", "Grain Tone HPF" };
    }

    static float raw(const Value* p) { return p->load(std::memory_order_relaxed); }
    static int bounded(const Value* p, int maximum) { return juce::jlimit(0, maximum, static_cast<int>(std::lround(raw(p)))); }
    static MotionValues bindMotion(juce::AudioProcessorValueTreeState& state, const juce::String& prefix)
    {
        return { state.getRawParameterValue(prefix + "_drift"), state.getRawParameterValue(prefix + "_unstable") };
    }

    static void bindRoutes(juce::AudioProcessorValueTreeState& state, const juce::String& prefix,
                           std::array<RouteValues, 4>& routes, bool perRouteToggle)
    {
        for (size_t slot = 0; slot < routes.size(); ++slot)
        {
            const auto suffix = slotId(slot);
            routes[slot] = { state.getRawParameterValue(prefix + "target_" + suffix),
                             state.getRawParameterValue(prefix + "depth_" + suffix),
                             perRouteToggle ? state.getRawParameterValue(prefix + "active_" + suffix) : nullptr };
        }
    }

    static std::array<Route, 4> readRoutes(const std::array<RouteValues, 4>& routes)
    {
        std::array<Route, 4> result;
        for (size_t i = 0; i < routes.size(); ++i)
        {
            const auto& r = routes[i];
            if (r.active == nullptr || raw(r.active) > .5f)
                result[i] = { bounded(r.target, modTargetCount - 1), raw(r.depth) * .005f };
        }
        return result;
    }

    struct EnvValues { const Value* source; const Value* attack; const Value* release; const Value* gain; };
    struct SeqValues
    {
        const Value* division = nullptr;
        const Value* length = nullptr;
        const Value* glide = nullptr;
        const Value* swing = nullptr;
        std::array<const Value*, 16> steps {};
    };

    std::array<LfoValues, 2> lfoParameters {};
    std::array<RouteValues, 4> macroRoutes {}, envRoutes {}, seqRoutes {}, xRoutes {}, yRoutes {};
    const Value* macro = nullptr;
    const Value* padX = nullptr;
    const Value* padY = nullptr;
    EnvValues env {};
    SeqValues seq {};
    MotionValues globalMotion {};
    std::array<MotionValues, motionPedalCount> localMotion {};
    std::array<MotionValues, motionPedalCount> secondaryMotion;
};

class Modulation
{
public:
    void prepare(double rate, ModulationSettings controls)
    {
        sampleRate = rate;
        tempo = 120;
        clocks = {};
        sequencer = {};
        envelope = 0;
        remaining = 0;
        beginBlock(controls, nullptr);
        current = evaluate(0);
    }

    void beginBlock(ModulationSettings config, const juce::AudioPlayHead* playHead)
    {
        controls = config;
        std::optional<double> ppq;
        double beatsPerBar = 4;
        if (playHead != nullptr)
        {
            if (const auto position = playHead->getPosition())
            {
                if (const auto bpm = position->getBpm(); bpm && std::isfinite(*bpm) && *bpm > 0)
                    tempo = *bpm;
                if (const auto time = position->getTimeSignature(); time && time->numerator > 0 && time->denominator > 0)
                    beatsPerBar = 4.0 * time->numerator / time->denominator;
                if (const auto p = position->getPpqPosition(); position->getIsPlaying() && p && std::isfinite(*p))
                    ppq = *p;
            }
        }
        bool clockRoutes = false;
        for (size_t i = 0; i < clocks.size(); ++i)
        {
            auto& clock = clocks[i];
            const auto& lfo = controls.lfos[i];
            constexpr std::array barMultipliers { 1.0, 2.0, 4.0, 8.0 };
            const auto beats = lfo.division < 6 ? beatsForDivision(lfo.division, 0)
                                               : beatsPerBar * barMultipliers[static_cast<size_t>(lfo.division - 6)];
            clock.step = lfo.sync ? tempo / (60.0 * beats * sampleRate) : lfo.rate / sampleRate;
            if (lfo.sync && ppq)
                clock.cycles = *ppq / beats;
            clock.routed = lfo.active && hasValidRoute(lfo.routes);
            clockRoutes = clockRoutes || clock.routed;
        }
        {
            constexpr std::array stepBeats { .125, .25, .5, 1.0, 2.0 };
            const auto beats = config.seq.division < 5 ? stepBeats[static_cast<size_t>(config.seq.division)] : beatsPerBar;
            sequencer.step = tempo / (60.0 * beats * sampleRate);
            if (ppq)
                sequencer.cycles = *ppq / beats;
            sequencer.routed = hasValidRoute(config.seq.routes);
        }
        const auto toCoefficient = [this](float milliseconds)
        {
            return static_cast<float>(std::exp(-1.0 / (sampleRate * juce::jmax(.05f, milliseconds) * .001)));
        };
        attackCoefficient = toCoefficient(config.env.attack);
        releaseCoefficient = toCoefficient(config.env.release);
        envelopeGain = juce::Decibels::decibelsToGain(config.env.gain);
        routed = clockRoutes || (config.macro != 0 && hasValidRoute(config.macroRoutes)) || sequencer.routed
              || (config.x != 0 && hasValidRoute(config.xRoutes)) || (config.y != 0 && hasValidRoute(config.yRoutes))
              || hasValidRoute(config.env.routes);
        if (!routed)
        {
            current = {};
            remaining = 0;
        }
    }

    bool update(int offset)
    {
        if (!routed || remaining-- > 0)
            return false;
        remaining = controlInterval - 1;
        const auto values = evaluate(offset);
        if (values == current)
            return false;
        current = values;
        return true;
    }

    // Feeds the envelope follower one sample's level (the louder channel, before any gain).
    void feed(float level)
    {
        const auto target = juce::jmin(1.0f, std::abs(level) * envelopeGain);
        const auto coefficient = target > envelope ? attackCoefficient : releaseCoefficient;
        envelope = target + coefficient * (envelope - target);
    }

    void endBlock(int samples)
    {
        // Publish what each source is doing at the end of the block for the editor's lamps and screens.
        for (size_t i = 0; i < clocks.size(); ++i)
            published[i].store(controls.lfos[i].active ? waveform(controls.lfos[i].shape, clocks[i].cycles, i) : 0.0f,
                               std::memory_order_relaxed);
        published[2].store(controls.macro, std::memory_order_relaxed);
        published[5].store(controls.x, std::memory_order_relaxed);
        published[6].store(controls.y, std::memory_order_relaxed);
        published[3].store(envelope, std::memory_order_relaxed);
        const auto [step, value] = sequence(sequencer.cycles);
        published[4].store(value, std::memory_order_relaxed);
        publishedStep.store(step, std::memory_order_relaxed);
        for (auto& clock : clocks)
            clock.cycles += clock.step * samples;
        sequencer.cycles += sequencer.step * samples;
    }

    ModulationActivity activity() const
    {
        return { published[0].load(std::memory_order_relaxed), published[1].load(std::memory_order_relaxed),
                 published[2].load(std::memory_order_relaxed), published[3].load(std::memory_order_relaxed),
                 published[4].load(std::memory_order_relaxed), published[5].load(std::memory_order_relaxed),
                 published[6].load(std::memory_order_relaxed), publishedStep.load(std::memory_order_relaxed) };
    }

    const ModOffsets& offsets() const { return current; }

    bool changes(ModTarget target) const
    {
        const auto matches = [target](const Route& route) { return route.target == target && route.depth != 0; };
        for (size_t i = 0; i < clocks.size(); ++i)
            if (clocks[i].routed && std::any_of(controls.lfos[i].routes.begin(), controls.lfos[i].routes.end(), matches))
                return true;
        return (controls.macro != 0 && std::any_of(controls.macroRoutes.begin(), controls.macroRoutes.end(), matches))
            || (controls.x != 0 && std::any_of(controls.xRoutes.begin(), controls.xRoutes.end(), matches))
            || (controls.y != 0 && std::any_of(controls.yRoutes.begin(), controls.yRoutes.end(), matches))
            || std::any_of(controls.env.routes.begin(), controls.env.routes.end(), matches)
            || std::any_of(controls.seq.routes.begin(), controls.seq.routes.end(), matches);
    }

private:
    struct Clock { double cycles = 0, step = 0; bool routed = false; };
    static constexpr int controlInterval = 32;

    static bool hasValidRoute(const std::array<Route, 4>& routes)
    {
        return std::any_of(routes.begin(), routes.end(), [](const Route& r) { return r.target != noTarget && r.depth != 0; });
    }

    static float random(std::int64_t cycle, size_t source)
    {
        auto value = static_cast<std::uint64_t>(cycle) ^ ((source + 1) * UINT64_C(0x9e3779b97f4a7c15));
        value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
        return static_cast<float>((value ^ (value >> 31)) >> 40) / 8388608.0f - 1.0f;
    }

    static float waveform(int shape, double cycles, size_t source)
    {
        const auto cycle = std::floor(cycles);
        const auto phase = static_cast<float>(cycles - cycle);
        switch (shape)
        {
            case 1: return 1.0f - 4.0f * std::abs(phase - .5f);
            case 2: return phase < .5f ? 1.0f : -1.0f;
            case 3: return random(static_cast<std::int64_t>(cycle), source);
            case 4:
            case 5:
            {
                const auto index = static_cast<std::int64_t>(cycle);
                const auto weight = shape == 4 ? phase : phase*phase*(3-2*phase);
                return std::lerp(random(index, source), random(index + 1, source), weight);
            }
            default: return static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * phase));
        }
    }

    ModOffsets evaluate(int offset) const
    {
        ModOffsets result {};
        const auto add = [&result](const std::array<Route, 4>& routes, float source)
        {
            for (const auto& route : routes)
                if (route.target != noTarget)
                    result[static_cast<size_t>(route.target)] += route.depth * source;
        };
        for (size_t i = 0; i < clocks.size(); ++i)
            if (clocks[i].routed)
                add(controls.lfos[i].routes, waveform(controls.lfos[i].shape, clocks[i].cycles + clocks[i].step * offset, i));
        add(controls.macroRoutes, controls.macro);
        add(controls.xRoutes, controls.x);
        add(controls.yRoutes, controls.y);
        add(controls.env.routes, envelope);
        if (sequencer.routed)
            add(controls.seq.routes, sequence(sequencer.cycles + sequencer.step * offset).second);
        return result;
    }

    // The step playing at `position` (counted in steps) and the sequencer's value there. Swing delays every second
    // step; glide slides from the previous step's value over that share of the step.
    std::pair<int, float> sequence(double position) const
    {
        const auto& seq = controls.seq;
        const auto length = juce::jlimit(1, 16, seq.length);
        const auto late = static_cast<double>(juce::jlimit(0.0f, .75f, seq.swing)) * .5;
        const auto pair = std::floor(position / 2.0);
        const auto within = position - pair * 2.0;
        const auto second = within >= 1.0 + late;
        const auto u = static_cast<float>(second ? (within - 1.0 - late) / (1.0 - late) : within / (1.0 + late));
        const auto count = static_cast<std::int64_t>(pair) * 2 + (second ? 1 : 0);
        const auto index = static_cast<int>(((count % length) + length) % length);
        const auto current = seq.steps[static_cast<size_t>(index)];
        if (seq.glide <= .001f)
            return { index, current };
        const auto previous = seq.steps[static_cast<size_t>((index + length - 1) % length)];
        const auto t = juce::jlimit(0.0f, 1.0f, u / seq.glide);
        return { index, std::lerp(previous, current, t * t * (3.0f - 2.0f * t)) };
    }

    ModulationSettings controls;
    std::array<Clock, 2> clocks;
    Clock sequencer;
    float envelope = 0, attackCoefficient = 0, releaseCoefficient = 0, envelopeGain = 1;
    std::array<std::atomic<float>, 7> published {};
    std::atomic<int> publishedStep { 0 };
    ModOffsets current {};
    double sampleRate = 48000, tempo = 120;
    int remaining = 0;
    bool routed = false;
};

struct MotionFrame { float drift = 0, positiveDrift = 0, unstable = 0; };
using MotionFrames = std::array<MotionFrame, motionPedalCount>;

class AnalogMotion
{
public:
    void prepare(double rate, const MotionSettings& controls)
    {
        sampleRate = rate;
        phase = 0;
        position = duration = 0;
        wait = rate * 3.5;
        random.setSeed(juce::Random::getSystemRandom().nextInt64());
        for (size_t i = 0; i < drift.size(); ++i)
        {
            initialise(drift[i], rate, controls[i].drift, .025);
            initialise(unstable[i], rate, controls[i].unstable, .025);
        }
        setTargets(controls);
    }

    void setTargets(const MotionSettings& controls)
    {
        active = false;
        for (size_t i = 0; i < drift.size(); ++i)
        {
            drift[i].setTargetValue(controls[i].drift);
            unstable[i].setTargetValue(controls[i].unstable);
            active = active || controls[i].drift != 0 || controls[i].unstable != 0
                            || drift[i].getCurrentValue() != 0 || unstable[i].getCurrentValue() != 0;
        }
    }

    MotionFrames next()
    {
        MotionFrames frames {};
        if (!active)
            return frames;

        const auto triangle = static_cast<float>(1 - 4 * std::abs(phase - .5));
        phase += .23 / sampleRate;
        phase -= std::floor(phase);
        auto burst = 0.0f;
        if (position < duration)
        {
            const auto envelope = static_cast<float>(.5 - .5 * std::cos(juce::MathConstants<double>::twoPi * position / duration));
            burst = sign * envelope;
            ++position;
        }
        else if (--wait <= 0)
        {
            position = 0;
            duration = sampleRate * (.95 + .95 * random.nextFloat());
            wait = sampleRate * (2.6 + 3.1 * random.nextFloat());
            sign = random.nextBool() ? 1.0f : -1.0f;
        }
        for (size_t i = 0; i < drift.size(); ++i)
        {
            const auto depth = drift[i].getNextValue();
            frames[i] = { depth * triangle, depth * (1 + triangle), unstable[i].getNextValue() * burst };
        }
        return frames;
    }

private:
    std::array<juce::SmoothedValue<float>, motionPedalCount> drift, unstable;
    juce::Random random;
    double sampleRate = 48000, phase = 0, position = 0, duration = 0, wait = 0;
    float sign = 1;
    bool active = false;
};
}
