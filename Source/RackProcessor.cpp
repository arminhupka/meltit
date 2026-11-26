#include "Effects.h"
#include "GlobalEffects.h"
#include "Modulation.h"
#include "PresetFile.h"
#include "RackEditor.h"
#include "RackPresets.h"
#include "PresetLibrary.h"
#include "ReverseReverb.h"
#include "SpatialEffects.h"
#include "TapeDsp.h"
#include "TapeTexture.h"
#include "TimeStretch.h"
#include "VhsDegrade.h"
#include "Granular.h"
#include "TapeTransport.h"
#include "GenerationLoss.h"

namespace
{
enum Parameter
{
    tapeActive, tapeDepth, tapeWowRate, tapeFlutterRate, tapeFlutterAmount, tapeAgedColor,
    tremoloActive, tremoloMode, tremoloDepth, tremoloRate, tremoloSync, tremoloNote, tremoloTiming,
    tremoloShape, tremoloStereo,
    saturationActive, saturationMode, saturationDrive, saturationFrequency, saturationTone, saturationMix,
    effectOrder, globalMix,
    resamplerSlot, chorusSlot, delaySlot,
    tapeNoise, tapeNoiseType, tapeNoiseGain, tapeDropoutRate, tapeDropoutDepth,
    resamplerActive, resamplerRate, resamplerDrive, resamplerPreFilter, resamplerPostFilter, resamplerJitter,
    chorusActive, chorusDepth, chorusRate, chorusWaveform, chorusSaturation, chorusAnalog, chorusStereo, chorusWidth, chorusMix,
    delayActive, delayType, delayWarp, delayTime, delaySync, delayNote, delayTiming, delayFeedback, delaySaturation,
    delayAnalog, delayHighpass, delayLowpass, delaySlope, delayModRate, delayModDepth, delayFreeze, delayMix,
    reverseSlot, reverbSlot,
    reverseActive, reverseType, reverseNote, reverseTiming, reverseForward, reverseHighpass, reverseLowpass,
    reversePitch, reverseMix,
    reverbActive, reverbType, reverbPreDelay, reverbEarlyLate, reverbSize, reverbDecay, reverbHighpass, reverbLowpass,
    reverbModRate, reverbModDepth, reverbSaturation, reverbMix,
    inputGain, outputGain, globalWidth, globalPitch, globalDetune,
    eqActive, eqLowFrequency, eqLowGain, eqMidFrequency, eqMidGain, eqHighFrequency, eqHighGain,
    stretchSlot,
    stretchActive, stretchEngine, stretchLength, stretchShape, stretchRatio, stretchStart, stretchLand,
    stretchCycle, stretchOverlap, stretchMix,
    vhsActive, vhsSpeed, vhsWear, vhsTracking, vhsWobble, vhsMix,
    transportStop, transportStopTime, transportStartTime, globalGenerations,
    grainActive, grainScale, grainSize, grainDensity, grainSync, grainDivision, grainPosition, grainPitch,
    grainPitchRandom, grainSizeRandom, grainDensityRandom, grainWidth, grainReverse, grainWindow, grainVoices,
    grainFeedback, grainHighpass, grainLowpass, grainStretch, grainStretchSpeed, grainRetrigger, grainMix,
    grainOffset, grainToneHighpass, grainToneLowpass,
    parameterCount
};

constexpr std::array<const char*, parameterCount> parameterIds
{
    "tape_active", "tape_depth", "tape_wow_rate", "tape_flutter_rate", "tape_flutter_amount", "tape_aged_color",
    "tremolo_active", "tremolo_mode", "tremolo_depth", "tremolo_rate", "tremolo_sync", "tremolo_note",
    "tremolo_timing", "tremolo_shape", "tremolo_stereo",
    "saturation_active", "saturation_mode", "saturation_drive", "saturation_frequency", "saturation_tone",
    "saturation_mix", "effect_order", "global_mix",
    "resampler_slot", "chorus_slot", "delay_slot",
    "tape_noise", "tape_noise_type", "tape_noise_gain", "tape_dropout_rate", "tape_dropout_depth",
    "resampler_active", "resampler_rate", "resampler_drive", "resampler_pre_filter", "resampler_post_filter", "resampler_jitter",
    "chorus_active", "chorus_depth", "chorus_rate", "chorus_waveform", "chorus_saturation", "chorus_analog",
    "chorus_stereo", "chorus_width", "chorus_mix",
    "delay_active", "delay_type", "delay_warp", "delay_time", "delay_sync", "delay_note", "delay_timing", "delay_feedback",
    "delay_saturation", "delay_analog", "delay_highpass", "delay_lowpass", "delay_slope", "delay_mod_rate",
    "delay_mod_depth", "delay_freeze", "delay_mix",
    "reverse_slot", "reverb_slot",
    "reverse_active", "reverse_type", "reverse_note", "reverse_timing", "reverse_forward", "reverse_highpass",
    "reverse_lowpass", "reverse_pitch", "reverse_mix",
    "reverb_active", "reverb_type", "reverb_pre_delay", "reverb_early_late", "reverb_size", "reverb_decay", "reverb_highpass",
    "reverb_lowpass", "reverb_mod_rate", "reverb_mod_depth", "reverb_saturation", "reverb_mix",
    "input_gain", "output_gain", "global_width", "global_pitch", "global_detune",
    "eq_active", "eq_low_freq", "eq_low_gain", "eq_mid_freq", "eq_mid_gain", "eq_high_freq", "eq_high_gain",
    "stretch_slot",
    "stretch_active", "stretch_engine", "stretch_length", "stretch_shape", "stretch_ratio", "stretch_start",
    "stretch_land", "stretch_cycle", "stretch_overlap", "stretch_mix",
    "vhs_active", "vhs_speed", "vhs_wear", "vhs_tracking", "vhs_wobble", "vhs_mix",
    "transport_stop", "transport_stop_time", "transport_start_time", "global_generations",
    "grain_active", "grain_scale", "grain_size", "grain_density", "grain_sync", "grain_division", "grain_position",
    "grain_pitch", "grain_pitch_random", "grain_size_random", "grain_density_random", "grain_width", "grain_reverse",
    "grain_window", "grain_voices", "grain_feedback", "grain_highpass", "grain_lowpass", "grain_stretch",
    "grain_stretch_speed", "grain_retrigger", "grain_mix", "grain_offset", "grain_tone_highpass", "grain_tone_lowpass"
};

enum Module
{
    tapeModule, tremoloModule, saturationModule, resamplerModule, chorusModule, delayModule,
    reverseModule, reverbModule, stretchModule, vhsModule, grainModule, moduleCount
};
using ChainOrder = std::array<Module, moduleCount>;
static_assert(moduleCount == rack::editorModuleCount);
static_assert(moduleCount * rack::cartridgeCopies <= 32, "leavingCartridges keeps one bit per instance");
constexpr std::array cartridgePrefixes { "tape_", "tremolo_", "saturation_", "resampler_", "chorus_",
                                         "delay_", "reverse_", "reverb_", "stretch_", "vhs_", "grain_" };
constexpr std::array enabledParameters { tapeActive, tremoloActive, saturationActive, resamplerActive,
                                        chorusActive, delayActive, reverseActive, reverbActive, stretchActive,
                                        vhsActive, grainActive };
constexpr uint64_t customRack = uint64_t{1} << 63;
constexpr int bitsPerSlot = 5;
constexpr uint64_t slotMask = (1u << bitsPerSlot) - 1u;
static_assert(rack::cartridgeSlots * bitsPerSlot < 63);
static_assert(rack::cartridgeCopies * moduleCount + 1 <= slotMask);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

juce::String secondaryId(const juce::String& id) { return "cart2_" + id; }

bool hasSecondary(Parameter key)
{
    const juce::StringRef id(parameterIds[key]);
    return !juce::String(id).endsWith("_slot")
        && std::any_of(cartridgePrefixes.begin(), cartridgePrefixes.end(), [id](const char* prefix)
           { return juce::String(id).startsWith(prefix); });
}

struct EffectBank
{
    tape::Processor tapeEffect;
    rack::TapeTexture tapeTexture;
    rack::Tremolo tremoloEffect;
    rack::Saturation saturationEffect;
    rack::Resampler resamplerEffect;
    rack::Chorus chorusEffect;
    rack::Delay delayEffect;
    rack::ReverseDelay reverseEffect;
    rack::Reverb reverbEffect;
    rack::TimeStretch stretchEffect;
    rack::VhsDegrade vhsEffect;
    rack::GranularCloud grainEffect;
    rack::AnalogMotion analogMotion;
    std::array<juce::SmoothedValue<float>, moduleCount> moduleWet;
    // A cartridge just put into the rack swells in from silence while its card drops out of the dock, instead of
    // cutting in at once. Read through `entryLevel`, which turns the straight ramp into a soft S.
    std::array<juce::SmoothedValue<float>, moduleCount> moduleEntry;
};

// How long a newly inserted effect takes to swell in, and how long the power switch takes to fade one in or out.
constexpr double entrySeconds = .7, exitSeconds = .4, powerSeconds = .12;

inline float entryLevel(juce::SmoothedValue<float>& entry)
{
    const auto t = entry.getNextValue();
    return t * t * (3.f - 2.f * t);
}

constexpr std::array<std::array<Module, 3>, 6> legacyOrders {{
    { tapeModule, tremoloModule, saturationModule }, { tapeModule, saturationModule, tremoloModule },
    { tremoloModule, tapeModule, saturationModule }, { tremoloModule, saturationModule, tapeModule },
    { saturationModule, tapeModule, tremoloModule }, { saturationModule, tremoloModule, tapeModule }
}};

juce::AudioProcessorValueTreeState::ParameterLayout parameters()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> secondary;
    const auto add = [&](Parameter key, const auto& make)
    {
        layout.add(make(juce::ParameterID { parameterIds[key], 1 }, juce::String{}));
        if (hasSecondary(key))
            secondary.push_back(make(juce::ParameterID { secondaryId(parameterIds[key]), 2 }, " 2"));
    };
    const auto toggle = [&](Parameter key, const char* name, bool value)
    {
        add(key, [&](auto id, const juce::String& suffix)
        {
            return std::make_unique<juce::AudioParameterBool>(id, name + suffix, value);
        });
    };
    const auto choice = [&](Parameter key, const char* name, const juce::StringArray& items, int value)
    {
        add(key, [&](auto id, const juce::String& suffix)
        {
            return std::make_unique<juce::AudioParameterChoice>(id, name + suffix, items, value);
        });
    };
    const auto number = [&](Parameter key, const char* name, const char* unit, float minimum,
                            float maximum, float step, float value, float centre = 0.0f)
    {
        add(key, [&](auto id, const juce::String& suffix)
        {
            const float max = key == delayFeedback && suffix.isNotEmpty() ? 100.0f : maximum;
            juce::NormalisableRange<float> range { minimum, max, step };
            if (centre > minimum && centre < max)
                range.setSkewForCentre(centre);
            auto attributes = juce::AudioParameterFloatAttributes().withLabel(unit);
            // Percentages and milliseconds read as whole numbers everywhere (knobs, readouts, host).
            if (juce::String(unit) == "%" || juce::String(unit) == "ms")
                attributes = attributes.withStringFromValueFunction([](float v, int) { return juce::String(juce::roundToInt(v)); });
            return std::make_unique<juce::AudioParameterFloat>(id, name + suffix, range, value, attributes);
        });
    };

    choice(effectOrder, "Effect Order", { "Tape > Tremolo > Saturation", "Tape > Saturation > Tremolo",
        "Tremolo > Tape > Saturation", "Tremolo > Saturation > Tape", "Saturation > Tape > Tremolo",
        "Saturation > Tremolo > Tape" }, 0);
    choice(resamplerSlot, "Resampler Slot", { "1", "2", "3", "4" }, 3);
    choice(chorusSlot, "Chorus Slot", { "1", "2", "3", "4", "5" }, 4);
    choice(delaySlot, "Delay Slot", { "1", "2", "3", "4", "5", "6" }, 5);
    number(globalMix, "Global Mix", "%", 0, 100, 0.1f, 100);

    toggle(tapeActive, "Tape Active", true);
    number(tapeDepth, "Tape Depth", "%", 0, 100, 0.1f, 10);
    number(tapeWowRate, "Tape Wow Rate", "Hz", 0.1f, 3, 0.01f, 0.6f);
    number(tapeFlutterRate, "Tape Flutter Rate", "Hz", 3, 30, 0.01f, 11);
    number(tapeFlutterAmount, "Tape Flutter Amount", "%", 0, 100, 0.1f, 10);
    number(tapeAgedColor, "Tape Aged Color", "%", 0, 100, 0.1f, 0);
    toggle(tapeNoise, "Tape Noise", false);
    choice(tapeNoiseType, "Tape Noise Type", { "Vinyl", "Vinyl 2", "Electronic Hiss", "Tape Noise", "Room Noise" }, 0);
    number(tapeNoiseGain, "Tape Noise Gain", "dB", -60, 0, .1f, -35);
    number(tapeDropoutRate, "Tape Dropout Rate", "Hz", .1f, 15, .01f, 2.3f);
    number(tapeDropoutDepth, "Tape Dropout Depth", "%", 0, 100, .1f, 0);

    toggle(tremoloActive, "Tremolo Active", false);
    choice(tremoloMode, "Tremolo Mode", { "Type A", "Type B" }, 0);
    number(tremoloDepth, "Tremolo Depth", "%", 0, 100, 0.1f, 50);
    number(tremoloRate, "Tremolo Rate", "Hz", 0.01f, 15, 0.001f, 0.5f, 1.88f);
    toggle(tremoloSync, "Tremolo Sync", true);
    choice(tremoloNote, "Tremolo Note", { "1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1/1" }, 3);
    choice(tremoloTiming, "Tremolo Timing", { "Straight", "Triplet", "Dotted" }, 0);
    number(tremoloShape, "Tremolo Shape", "%", 0, 100, 0.1f, 50);
    toggle(tremoloStereo, "Tremolo Stereo", false);

    toggle(saturationActive, "Saturation Active", false);
    choice(saturationMode, "Saturation Mode", { "Soft", "Saturate" }, 1);
    number(saturationDrive, "Saturation Drive", "dB", 0, 30.9f, 0.01f, 0, 14.4f);
    number(saturationFrequency, "Saturation Frequency", "Hz", 50, 10000, 1, 499, 499);
    number(saturationTone, "Saturation Tone", "%", 0, 100, 0.1f, 50);
    number(saturationMix, "Saturation Mix", "%", 0, 100, 0.1f, 100);

    toggle(resamplerActive, "Resampler Active", false);
    number(resamplerRate, "Resampler Rate", "Hz", 44, 44100, 1.0f, 44100, 5551);
    number(resamplerDrive, "Resampler Drive", "dB", 0, 12, .01f, 0, 8);
    toggle(resamplerPreFilter, "Resampler Pre Filter", true);
    toggle(resamplerPostFilter, "Resampler Post Filter", true);
    number(resamplerJitter, "Resampler Jitter", "%", 0, 100, .1f, 0);

    toggle(chorusActive, "Chorus Active", false);
    number(chorusDepth, "Chorus Depth", "%", 0, 100, .1f, 40);
    number(chorusRate, "Chorus Rate", "Hz", .01f, 5, .001f, .25f, .634f);
    choice(chorusWaveform, "Chorus Waveform", { "Triangle", "Sine" }, 0);
    number(chorusSaturation, "Chorus Saturation", "dB", 0, 18, .01f, 3);
    number(chorusAnalog, "Chorus Analog", "%", 0, 100, .1f, 0);
    toggle(chorusStereo, "Chorus Stereo", true);
    number(chorusWidth, "Chorus Width", "%", 0, 100, .1f, 30);
    number(chorusMix, "Chorus Mix", "%", 0, 100, .1f, 30);

    toggle(delayActive, "Delay Active", false);
    choice(delayType, "Delay Type", { "Standard", "PingPong" }, 0);
    number(delayWarp, "Delay Warp", "x", .75f, 1.5f, .001f, 1);
    number(delayTime, "Delay Time", "ms", 1, 2500, .1f, 374, 313);
    toggle(delaySync, "Delay Sync", true);
    choice(delayNote, "Delay Note", { "1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1/1" }, 3);
    choice(delayTiming, "Delay Timing", { "Straight", "Triplet", "Dotted" }, 0);
    number(delayFeedback, "Delay Feedback", "%", 0, 200, .1f, 50);
    number(delaySaturation, "Delay Saturation", "dB", 0, 18, .01f, 3);
    number(delayAnalog, "Delay Analog", "%", 0, 100, .1f, 0);
    number(delayHighpass, "Delay HPF", "Hz", 10, 1000, 1, 10, 257);
    number(delayLowpass, "Delay LPF", "Hz", 500, 22000, 1, 10000, 1171);
    choice(delaySlope, "Delay LPF Slope", { "12 dB", "24 dB" }, 1);
    number(delayModRate, "Delay Mod Rate", "Hz", .1f, 15, .001f, .4f, 1.96f);
    number(delayModDepth, "Delay Mod Depth", "%", 0, 100, .1f, 0);
    toggle(delayFreeze, "Delay Freeze", false);
    number(delayMix, "Delay Mix", "%", 0, 100, .1f, 50);

    choice(reverseSlot, "Reverse Slot", { "1", "2", "3", "4", "5", "6", "7" }, 6);
    choice(reverbSlot, "Reverb Slot", { "1", "2", "3", "4", "5", "6", "7", "8" }, 7);

    toggle(reverseActive, "Reverse Active", false);
    choice(reverseType, "Reverse Type", { "Standard", "PingPong" }, 0);
    choice(reverseNote, "Reverse Note", { "1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1/1" }, 5);
    choice(reverseTiming, "Reverse Timing", { "Straight", "Triplet", "Dotted" }, 0);
    number(reverseForward, "Reverse Forward", "%", 0, 100, .1f, 25);
    number(reverseHighpass, "Reverse HPF", "Hz", 10, 1000, 1, 10, 257);
    number(reverseLowpass, "Reverse LPF", "Hz", 500, 22000, 1, 10000, 1171);
    number(reversePitch, "Reverse Pitch", "semi", -12, 12, 1, 0);
    number(reverseMix, "Reverse Mix", "%", 0, 100, .1f, 50);

    toggle(reverbActive, "Reverb Active", false);
    choice(reverbType, "Reverb Type", { "Room", "Plate", "Hall" }, 1);
    number(reverbPreDelay, "Reverb Pre Delay", "ms", 0, 200, .1f, 0, 50);
    number(reverbEarlyLate, "Reverb Early Late", "%", 0, 100, .1f, 50);
    number(reverbSize, "Reverb Size", "%", 0, 100, .1f, 50);
    number(reverbDecay, "Reverb Decay", "s", .1f, 60, .01f, 2, 7.6f);
    number(reverbHighpass, "Reverb HPF", "Hz", 10, 500, 1, 10, 132);
    number(reverbLowpass, "Reverb LPF", "Hz", 500, 22000, 1, 10000, 1171);
    number(reverbModRate, "Reverb Mod Rate", "Hz", .1f, 5, .001f, .5f, .713f);
    number(reverbModDepth, "Reverb Mod Depth", "%", 0, 100, .1f, 0);
    number(reverbSaturation, "Reverb Saturation", "dB", 0, 12, .01f, 0);
    number(reverbMix, "Reverb Mix", "%", 0, 100, .1f, 25);

    choice(stretchSlot, "Stretch Slot", { "1", "2", "3", "4", "5", "6", "7", "8", "9" }, 8);
    toggle(stretchActive, "Stretch Active", false);
    choice(stretchEngine, "Stretch Engine", { "Smooth", "Nuskool", "OG Stretch", "Hard Stretch" }, 0);
    choice(stretchLength, "Stretch Length", { "1/4 Bar", "1/2 Bar", "1 Bar", "2 Bars", "4 Bars" }, 2);
    choice(stretchShape, "Stretch Shape", { "Hold", "Ramp Up", "Ramp Down", "Swell", "Steps" }, 0);
    number(stretchRatio, "Stretch Ratio", "x", 1, 20, .01f, 2, 4);
    number(stretchStart, "Stretch Start", "%", 0, 100, .1f, 0);
    number(stretchLand, "Stretch Land", "%", 0, 100, .1f, 75);
    number(stretchCycle, "Stretch Cycle", "smp", 20, 4000, 1, 998, 700);
    number(stretchOverlap, "Stretch Overlap", "%", 0, 50, .1f, 25);
    number(stretchMix, "Stretch Mix", "%", 0, 100, .1f, 100);
    toggle(vhsActive, "VHS Active", false);
    choice(vhsSpeed, "VHS Speed", { "SP", "LP", "EP" }, 0);
    number(vhsWear, "VHS Wear", "%", 0, 100, .1f, 35);
    number(vhsTracking, "VHS Tracking", "%", 0, 100, .1f, 20);
    number(vhsWobble, "VHS Wobble", "%", 0, 100, .1f, 30);
    number(vhsMix, "VHS Mix", "%", 0, 100, .1f, 100);
    toggle(grainActive, "Grain Active", false);
    choice(grainScale, "Grain Scale", rack::GranularCloud::scaleNames(), 1);
    number(grainSize, "Grain Size", "ms", 10, 1000, 1, 120, 120);
    number(grainDensity, "Grain Density", "Hz", .5f, 100, .1f, 12, 12);
    toggle(grainSync, "Grain Sync", false);
    choice(grainDivision, "Grain Division", rack::GranularCloud::divisionNames(), 10);
    number(grainPosition, "Grain Position", "ms", 0, 2000, .1f, 20, 300);
    number(grainPitch, "Grain Pitch", "", -24, 24, 1, 0);
    number(grainPitchRandom, "Grain Pitch Random", "%", 0, 100, .1f, 0);
    number(grainSizeRandom, "Grain Size Random", "%", 0, 100, .1f, 0);
    number(grainDensityRandom, "Grain Density Random", "%", 0, 100, .1f, 0);
    number(grainWidth, "Grain Width", "%", 0, 100, .1f, 50);
    toggle(grainReverse, "Grain Reverse", false);
    number(grainWindow, "Grain Window", "%", 0, 100, .1f, 100);
    number(grainVoices, "Grain Voices", "", 1, 16, 1, 8);
    number(grainFeedback, "Grain Feedback", "%", 0, 100, .1f, 0);
    number(grainHighpass, "Grain HPF", "Hz", 20, 2000, 1, 80, 200);
    number(grainLowpass, "Grain LPF", "Hz", 20, 20000, 1, 9000, 2000);
    toggle(grainStretch, "Grain Stretch", false);
    number(grainStretchSpeed, "Grain Stretch Speed", "%", 0, 100, .1f, 50);
    choice(grainRetrigger, "Grain Retrigger", rack::GranularCloud::retriggerNames(), 0);
    number(grainOffset, "Grain Offset", "ms", 0, 250, .1f, 0, 40);
    number(grainToneHighpass, "Grain Tone HPF", "Hz", 20, 5000, 1, 20, 300);
    number(grainToneLowpass, "Grain Tone LPF", "Hz", 200, 20000, 1, 20000, 3000);
    number(grainMix, "Grain Mix", "%", 0, 100, .1f, 50);
    toggle(transportStop, "Tape Stop", false);
    number(transportStopTime, "Tape Stop Time", "ms", 50, 4000, 1, 900, 700);
    number(transportStartTime, "Tape Start Time", "ms", 20, 2000, 1, 350, 300);
    number(globalGenerations, "Global Generations", "x", 1, 10, .01f, 1);
    rack::ModulationParameters::add(layout);

    number(inputGain, "Input Gain", "dB", -24, 24, .01f, 0);
    number(outputGain, "Output Gain", "dB", -24, 24, .01f, 0);
    number(globalWidth, "Global Width", "%", 0, 200, .1f, 100);
    number(globalPitch, "Global Pitch", "semi", -12, 12, 1, 0);
    number(globalDetune, "Global Detune", "cents", -50, 50, 1, 0);
    toggle(eqActive, "EQ Active", true);
    number(eqLowFrequency, "EQ Low Freq", "Hz", 80, 500, 1, 250, 132);
    number(eqLowGain, "EQ Low Gain", "dB", -15, 15, .01f, 0);
    number(eqMidFrequency, "EQ Mid Freq", "Hz", 500, 3000, 1, 2000, 812);
    number(eqMidGain, "EQ Mid Gain", "dB", -15, 15, .01f, 0);
    number(eqHighFrequency, "EQ High Freq", "Hz", 3000, 15000, 1, 8000, 4500);
    number(eqHighGain, "EQ High Gain", "dB", -15, 15, .01f, 0);
    for (auto& parameter : secondary)
        layout.add(std::move(parameter));
    return layout;
}

class RackProcessor final : public juce::AudioProcessor, private rack::EditorModel
{
public:
    RackProcessor()
        : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                          .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                                          .withInput("Sidechain", juce::AudioChannelSet::stereo(), false)),
          state(*this, nullptr, "TapeMultiFxSettings", parameters()), modulationParameters(state)
    {
        for (size_t i = 0; i < parameterIds.size(); ++i)
        {
            values[0][i] = state.getRawParameterValue(parameterIds[i]);
            values[1][i] = state.getRawParameterValue(secondaryId(parameterIds[i]));
        }
        defaultState = state.copyState();
        // Unpack the built-in presets into the user's data folder when they are not there yet.
        juce::Thread::launch([] { rack::presetLibraryDirectory(); });
    }

    void prepareToPlay(double sampleRate, int maximumBlockSize) override
    {
        const auto channels = juce::jlimit(1, 2, getTotalNumOutputChannels());
        const auto capacity = juce::jlimit(32, 1024, maximumBlockSize);
        wetBuffer.setSize(channels, capacity);
        blendBuffer.resize(static_cast<size_t>(capacity));
        globals.prepare(sampleRate, channels, globalControls());
        displayTempo.store(120.0f, std::memory_order_relaxed);
        for (auto& peak : outputPeaks)
            peak.store(0.0f, std::memory_order_relaxed);
        modulation.prepare(sampleRate, modulationParameters.settings());
        const auto& routed = modulation.offsets();
        for (int layer = 0; layer < rack::cartridgeCopies; ++layer)
        {
            auto& bank = banks[layer];
            bank.analogMotion.prepare(sampleRate, modulationParameters.motion(layer));
            bank.tapeEffect.prepare(sampleRate, tapeControls(routed, layer));
            bank.tapeTexture.prepare(sampleRate, textureControls(layer));
            bank.tremoloEffect.prepare(sampleRate, tremoloControls(routed, layer));
            bank.saturationEffect.prepare(sampleRate, saturationControls(routed, layer));
            bank.resamplerEffect.prepare(sampleRate, resamplerControls(routed, layer));
            bank.chorusEffect.prepare(sampleRate, chorusControls(routed, layer));
            bank.delayEffect.prepare(sampleRate, delayControls(routed, layer));
            bank.reverseEffect.prepare(sampleRate, reverseControls(routed, layer));
            bank.reverbEffect.prepare(sampleRate, reverbControls(routed, layer));
            bank.stretchEffect.prepare(sampleRate, stretchControls(routed, layer));
            bank.vhsEffect.prepare(sampleRate, vhsControls(routed, layer));
            bank.grainEffect.prepare(sampleRate, grainControls(routed, layer));
            for (size_t i = 0; i < enabledParameters.size(); ++i)
            {
                rack::initialise(bank.moduleWet[i], sampleRate, value(enabledParameters[i], layer), powerSeconds);
                // Whatever is already in the rack when playback starts plays at once.
                rack::initialise(bank.moduleEntry[i], sampleRate, 1.0f, entrySeconds);
            }
        }
        rack::initialise(mix, sampleRate, percent(globalMix));
        transport.prepare(sampleRate, channels);
        generationLoss.prepare(sampleRate, value(globalGenerations));
        heldNotes = 0;
        rack::initialise(orderFade, sampleRate, 1.0f, 0.005);
        activeOrder = processingOrder();
    }

    void releaseResources() override {}

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        const auto output = layouts.getMainOutputChannelSet();
        // The sidechain, which only feeds the envelope follower, may be off, mono or stereo.
        const auto side = layouts.inputBuses.size() > 1 ? layouts.getChannelSet(true, 1) : juce::AudioChannelSet::disabled();
        return output == layouts.getMainInputChannelSet()
            && (output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo())
            && (side.isDisabled() || side == juce::AudioChannelSet::mono() || side == juce::AudioChannelSet::stereo());
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        // The main bus carries the audio; an enabled sidechain bus only feeds the envelope follower.
        auto audio = getBusBuffer(buffer, false, 0);
        const auto* sideBus = getBusCount(true) > 1 ? getBus(true, 1) : nullptr;
        const auto sideChannels = sideBus != nullptr && sideBus->isEnabled() ? sideBus->getNumberOfChannels() : 0;
        const auto sideOffset = sideChannels > 0 ? getChannelIndexInProcessBlockBuffer(true, 1, 0) : 0;
        // Any held note stops the tape, for playing tape stops from a keyboard or a MIDI clip.
        for (const auto metadata : midi)
        {
            const auto message = metadata.getMessage();
            if (message.isNoteOn())
                ++heldNotes;
            else if (message.isNoteOff())
                heldNotes = juce::jmax(0, heldNotes - 1);
            else if (message.isAllNotesOff() || message.isAllSoundOff())
                heldNotes = 0;
        }
        midi.clear();
        if (audio.getNumSamples() == 0 || audio.getNumChannels() == 0)
            return;

        juce::ScopedNoDenormals noDenormals;
        const auto* playHead = getPlayHead();
        modulation.beginBlock(modulationParameters.settings(), playHead);
        const auto& routed = modulation.offsets();
        const auto routingVersion = orderEdit.load(std::memory_order_acquire);
        auto newOrder = processingOrder();
        if ((routingVersion & 1u) || routingVersion != orderEdit.load(std::memory_order_acquire))
            newOrder = activeOrder;
        const auto hasSecondary = [](const rack::CartridgeOrder& order)
        {
            return std::any_of(order.begin(), order.end(), [](int item) { return item >= moduleCount; });
        };
        const int layers = (hasSecondary(newOrder) || hasSecondary(activeOrder)) ? 2 : 1;
        for (int layer = 0; layer < layers; ++layer)
        {
            auto& bank = banks[layer];
            bank.analogMotion.setTargets(modulationParameters.motion(layer));
            bank.tapeEffect.setTargets(tapeControls(routed, layer));
            bank.tapeTexture.setTargets(textureControls(layer));
            bank.tremoloEffect.beginBlock(tremoloControls(routed, layer), playHead);
            bank.saturationEffect.setTargets(saturationControls(routed, layer));
            bank.resamplerEffect.setTargets(resamplerControls(routed, layer));
            bank.chorusEffect.setTargets(chorusControls(routed, layer));
            bank.delayEffect.beginBlock(delayControls(routed, layer), playHead);
            bank.reverseEffect.beginBlock(reverseControls(routed, layer), playHead, audio.getNumSamples());
            bank.reverbEffect.setTargets(reverbControls(routed, layer));
            bank.stretchEffect.beginBlock(stretchControls(routed, layer), playHead, audio.getNumSamples());
            bank.vhsEffect.setTargets(vhsControls(routed, layer));
            bank.grainEffect.beginBlock(grainControls(routed, layer), playHead);
            for (size_t i = 0; i < enabledParameters.size(); ++i)
                bank.moduleWet[i].setTargetValue(value(enabledParameters[i], layer));
        }
        displayTempo.store(static_cast<float>(banks[0].delayEffect.currentTempo()), std::memory_order_relaxed);
        globals.setTargets(globalControls());
        mix.setTargetValue(percent(globalMix));
        generationLoss.setTarget(value(globalGenerations));
        transport.setControls({ value(transportStop) > .5f || heldNotes > 0, scrubbing.load(std::memory_order_relaxed),
                                scrubSpeed.load(std::memory_order_relaxed), value(transportStopTime) * .001f,
                                value(transportStartTime) * .001f });
        // Effects leaving the chain, or whose cartridge is being pulled out, fade to silence first; the chain is
        // only rebuilt (through its short dip to dry) once they are quiet. One that stays after all comes back.
        const auto leaving = leavingCartridges.load(std::memory_order_acquire);
        auto departingSounds = false;
        for (const auto instance : activeOrder)
        {
            if (instance < 0)
                break;
            auto& entry = banks[instance / moduleCount].moduleEntry[instance % moduleCount];
            const auto stays = std::find(newOrder.begin(), newOrder.end(), instance) != newOrder.end();
            const auto fadingOut = !stays || (leaving & (1u << static_cast<unsigned>(instance))) != 0;
            if (fadingOut && entry.getTargetValue() > 0.0f)
                rampEntry(entry, 0.0f, exitSeconds);
            else if (!fadingOut && entry.getTargetValue() < 1.0f)
                rampEntry(entry, 1.0f, entrySeconds);
            if (!stays && entry.getCurrentValue() > 0.0f)
                departingSounds = true;
        }
        orderFade.setTargetValue(newOrder == activeOrder || departingSounds ? 1.0f : 0.0f);
        // A mark for a cartridge no longer in the rack has done its job; dropping it lets the same effect come back.
        auto present = 0u;
        for (const auto instance : newOrder)
        {
            if (instance < 0)
                break;
            present |= 1u << static_cast<unsigned>(instance);
        }
        if ((leaving & ~present) != 0)
            leavingCartridges.fetch_and(present, std::memory_order_acq_rel);
        const auto channelCount = audio.getNumChannels();
        auto* const* data = audio.getArrayOfWritePointers();
        const auto fromSidechain = sideChannels > 0 && modulationParameters.settings().env.sidechain;
        const auto* sideLeft = fromSidechain ? buffer.getReadPointer(sideOffset) : nullptr;
        const auto* sideRight = fromSidechain ? buffer.getReadPointer(sideOffset + juce::jmin(1, sideChannels - 1)) : nullptr;
        auto* const* wet = wetBuffer.getArrayOfWritePointers();
        for (int start = 0; start < audio.getNumSamples(); start += wetBuffer.getNumSamples())
        {
            const auto count = juce::jmin(wetBuffer.getNumSamples(), audio.getNumSamples() - start);
            for (int j = 0; j < count; ++j)
            {
                const auto i = start + j;
                // The envelope follower hears the sidechain when it is chosen and connected, else the input.
                modulation.feed(fromSidechain ? juce::jmax(std::abs(sideLeft[i]), std::abs(sideRight[i]))
                                              : juce::jmax(std::abs(data[0][i]), std::abs(data[juce::jmin(1, channelCount - 1)][i])));
                if (modulation.update(i))
                    for (int layer = 0; layer < layers; ++layer)
                        applyRoutedTargets(routed, layer);
                std::array<rack::MotionFrames, rack::cartridgeCopies> motion {};
                for (int layer = 0; layer < layers; ++layer)
                    motion[layer] = banks[layer].analogMotion.next();
                // An order change fades the wet signal through dry before reusing module state.
                const auto orderLevel = orderFade.getNextValue();
                if (newOrder != activeOrder && orderLevel <= 0.0f)
                {
                    // Effects that were not in the chain before start silent and swell in.
                    for (const auto instance : newOrder)
                    {
                        if (instance < 0)
                            break;
                        if (std::find(activeOrder.begin(), activeOrder.end(), instance) == activeOrder.end())
                        {
                            auto& entry = banks[instance / moduleCount].moduleEntry[instance % moduleCount];
                            entry.setCurrentAndTargetValue(0.0f);
                            rampEntry(entry, 1.0f, entrySeconds);
                        }
                    }
                    activeOrder = newOrder;
                    orderFade.setTargetValue(1.0f);
                }

                std::array frame { data[0][i], channelCount == 2 ? data[1][i] : 0.0f };
                globals.applyInput(frame, channelCount);
                // The reorder dip passes only the effect chain through dry; input and output gain, EQ, width and
                // pitch stay applied throughout, so a reorder never jumps the level.
                auto chained = frame;
                renderChainFrame(chained, channelCount, motion);
                rack::mixFrames(frame, chained, channelCount, orderLevel);
                generationLoss.process(frame, channelCount);
                blendBuffer[static_cast<size_t>(j)] = mix.getNextValue();
                for (int c = 0; c < channelCount; ++c)
                    wet[c][j] = frame[static_cast<size_t>(c)];
            }

            globals.applyOutput(wetBuffer, channelCount, count);
            for (int c = 0; c < channelCount; ++c)
            {
                for (int j = 0; j < count; ++j)
                {
                    auto& output = data[c][start + j];
                    output = std::lerp(output, wet[c][j], blendBuffer[static_cast<size_t>(j)]);
                }
            }
        }
        transport.process(audio, audio.getNumSamples());
        modulation.endBlock(audio.getNumSamples());
        for (size_t c = 0; c < outputPeaks.size(); ++c)
        {
            const auto channel = juce::jmin(static_cast<int>(c), channelCount - 1);
            const auto peak = audio.getMagnitude(channel, 0, audio.getNumSamples());
            auto& held = outputPeaks[c];
            auto previous = held.load(std::memory_order_relaxed);
            if (std::isfinite(peak) && peak > previous
                && !held.compare_exchange_strong(previous, peak, std::memory_order_relaxed))
                held.store(peak, std::memory_order_relaxed);
        }
    }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return rack::makeCartridgeEditor(*this, state, *this); }
    const juce::String getName() const override { return "MELTIT"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override
    {
        double tail = .25;
        for (const auto instance : processingOrder())
        {
            if (instance < 0)
                break;
            const auto layer = instance / moduleCount;
            const auto type = instance % moduleCount;
            if (value(enabledParameters[type], layer) < .5f)
                continue;
            if (type == delayModule)
            {
                const auto delayTail = value(delayFreeze, layer) > .5f || value(delayFeedback, layer) >= 100.f
                    ? 60.0 : 30.0;
                tail = std::max(tail, delayTail);
            }
            if (type == reverseModule)
                tail = std::max(tail, banks[layer].reverseEffect.tailSeconds(reverseControls({}, layer)));
            if (type == reverbModule)
                tail = std::max(tail, static_cast<double>(value(reverbDecay, layer)) + .5);
            if (type == stretchModule)
                tail = std::max(tail, banks[layer].stretchEffect.tailSeconds());
            if (type == grainModule)
                tail = std::max(tail, banks[layer].grainEffect.tailSeconds());
        }
        return tail + (globalPitchAmount() == 0 ? 0.0 : globals.pitchTailSeconds());
    }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& output) override
    {
        auto updated = state.copyState();
        updated.setProperty("stateVersion", 7, nullptr);
        const auto order = cartridgeState.load(std::memory_order_acquire);
        if (order != 0)
            updated.setProperty("cartridgeOrder", juce::String::toHexString(static_cast<juce::int64>(order)), nullptr);
        const auto identity = presetIdentity();
        if (identity.origin >= 0)
            updated.setProperty("presetOrigin", identity.origin, nullptr);
        if (identity.variation >= 0)
            updated.setProperty("presetVariation", identity.variation, nullptr);
        updated.setProperty("editorScale", static_cast<double>(editorScale()), nullptr);
        if (const auto xml = updated.createXml())
            copyXmlToBinary(*xml, output);
    }

    void setStateInformation(const void* input, int size) override
    {
        orderEdit.fetch_add(1, std::memory_order_acq_rel);
        restoreState(input, size);
        orderEdit.fetch_add(1, std::memory_order_release);
    }

private:
    bool restoreState(const void* input, int size)
    {
        if (const auto xml = getXmlFromBinary(input, size); xml && xml->hasTagName(state.state.getType()))
        {
            const auto restored = juce::ValueTree::fromXml(*xml);
            const auto version = static_cast<int>(restored.getProperty("stateVersion", 1));
            if (version < 1 || version > 7)
                return false;

            auto migrated = defaultState.createCopy();
            for (const auto& parameter : restored)
            {
                auto target = migrated.getChildWithProperty("id", parameter.getProperty("id"));
                if (target.isValid() && parameter.hasProperty("value"))
                    target.setProperty("value", parameter.getProperty("value"), nullptr);
            }
            auto order = static_cast<uint64_t>(restored.getProperty("cartridgeOrder").toString().getHexValue64());
            // Racks saved with fewer effects numbered second copies from that count; they now start later.
            if (version < 7)
                order = withNewNumbering(order, version < 6 ? vhsModule : grainModule);
            if (restored.hasProperty("editorScale"))
                setEditorScale(static_cast<float>(static_cast<double>(restored.getProperty("editorScale"))));
            state.replaceState(migrated);
            cartridgeState.store(order & customRack ? encodeCartridges(decodeCartridges(order)) : 0,
                                 std::memory_order_release);
            const auto origin = static_cast<int>(restored.getProperty("presetOrigin", -1));
            const auto isPreset = origin >= 0 && origin < static_cast<int>(rack::presets().size());
            const auto isGenerated = origin >= rack::generatedOriginBase
                && origin < rack::generatedOriginBase + static_cast<int>(rack::tagNames.size());
            rack::PresetIdentity identity { isPreset || isGenerated ? origin : -1 };
            if (isGenerated)
            {
                const auto variation = static_cast<int>(restored.getProperty("presetVariation", -1));
                if (variation >= 0)
                    identity.variation = variation;
            }
            currentPreset.store(identity, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    // Racks saved before the VHS cartridge numbered second copies from 9 (one past the nine effects then), and
    // racks saved before the Grain cartridge from 10; they now start at moduleCount.
    static uint64_t withNewNumbering(uint64_t packed, int previousModules)
    {
        if ((packed & customRack) == 0)
            return packed;
        auto renumbered = customRack;
        for (size_t i = 0; i < rack::cartridgeSlots; ++i)
        {
            auto item = static_cast<int>((packed >> (bitsPerSlot * i)) & slotMask) - 2;
            if (item >= previousModules)
                item += moduleCount - previousModules;
            if (item + 2 > static_cast<int>(slotMask))
                item = rack::noCartridge;
            renumbered |= static_cast<uint64_t>(item + 2) << (bitsPerSlot * i);
            if (item == rack::noCartridge)
                break;
        }
        return renumbered;
    }

    static uint64_t encodeCartridges(const rack::CartridgeOrder& order)
    {
        auto packed = customRack;
        for (size_t i = 0; i < order.size() && order[i] != rack::noCartridge; ++i)
            packed |= static_cast<uint64_t>(order[i] + 2) << (bitsPerSlot * i);
        return packed;
    }

    static rack::CartridgeOrder decodeCartridges(uint64_t packed)
    {
        rack::CartridgeOrder ordered;
        ordered.fill(rack::noCartridge);
        std::array<bool, moduleCount * rack::cartridgeCopies> used {};
        size_t destination = 0;
        for (size_t i = 0; i < ordered.size(); ++i)
        {
            const auto item = static_cast<int>((packed >> (bitsPerSlot * i)) & slotMask) - 2;
            if (item == rack::noCartridge)
                break;
            if (item == rack::emptyCartridge)
                ordered[destination++] = item;
            else if (item < static_cast<int>(used.size()) && !std::exchange(used[item], true))
                ordered[destination++] = item;
        }
        return ordered;
    }

    rack::CartridgeOrder cartridges() const override
    {
        const auto packed = cartridgeState.load(std::memory_order_acquire);
        if (packed != 0)
            return decodeCartridges(packed);

        rack::CartridgeOrder order;
        order.fill(rack::noCartridge);
        size_t position = 0;
        for (const auto type : readOrder())
            if (value(enabledParameters[type]) >= .5f)
                order[position++] = type;
        return order;
    }

    rack::CartridgeOrder processingOrder() const
    {
        const auto packed = cartridgeState.load(std::memory_order_acquire);
        rack::CartridgeOrder order;
        order.fill(rack::noCartridge);
        if (packed == 0)
        {
            const auto legacy = readOrder();
            std::copy(legacy.begin(), legacy.end(), order.begin());
        }
        else
        {
            const auto visible = decodeCartridges(packed);
            std::copy_if(visible.begin(), visible.end(), order.begin(), [](int item) { return item >= 0; });
        }
        return order;
    }

    static int slotCount(const rack::CartridgeOrder& order)
    {
        return static_cast<int>(std::find(order.begin(), order.end(), rack::noCartridge) - order.begin());
    }

    void publishCartridges(const rack::CartridgeOrder& order)
    {
        cartridgeState.store(encodeCartridges(order), std::memory_order_release);
        updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
    }

    juce::RangedAudioParameter& powerParameter(int instance)
    {
        const auto* id = parameterIds[enabledParameters[instance % moduleCount]];
        return *state.getParameter(instance < moduleCount ? juce::String(id) : secondaryId(id));
    }

    bool insertCartridge(int type, int position) override
    {
        auto order = cartridges();
        const auto count = slotCount(order);
        if (type < rack::emptyCartridge || type >= moduleCount || position < 0 || position > count
            || position >= rack::cartridgeSlots || (position < count && order[position] != rack::emptyCartridge))
            return false;

        auto instance = type;
        if (type >= 0)
        {
            while (instance < moduleCount * rack::cartridgeCopies
                   && std::find(order.begin(), order.end(), instance) != order.end())
                instance += moduleCount;
            if (instance >= moduleCount * rack::cartridgeCopies)
                return false;
            orderEdit.fetch_add(1, std::memory_order_acq_rel);
            setParameter(powerParameter(instance), 1.0f);
        }
        order[position] = instance;
        cartridgeState.store(encodeCartridges(order), std::memory_order_release);
        if (type >= 0)
            orderEdit.fetch_add(1, std::memory_order_release);
        updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
        return true;
    }

    void fadeOutCartridge(int instance, bool leaving) override
    {
        if (instance < 0 || instance >= moduleCount * rack::cartridgeCopies)
            return;
        const auto bit = 1u << static_cast<unsigned>(instance);
        if (leaving)
            leavingCartridges.fetch_or(bit, std::memory_order_release);
        else
            leavingCartridges.fetch_and(~bit, std::memory_order_release);
    }

    void ejectCartridge(int position) override
    {
        auto order = cartridges();
        if (position < 0 || position >= slotCount(order))
            return;
        std::move(order.begin() + position + 1, order.end(), order.begin() + position);
        order.back() = rack::noCartridge;
        publishCartridges(order);
    }

    void moveCartridge(int source, int destination) override
    {
        auto order = cartridges();
        const auto count = slotCount(order);
        if (source < 0 || destination < 0 || source >= count || destination >= count || source == destination)
            return;
        if (source < destination)
            std::rotate(order.begin() + source, order.begin() + source + 1, order.begin() + destination + 1);
        else
            std::rotate(order.begin() + destination, order.begin() + source, order.begin() + source + 1);
        publishCartridges(order);
    }

    void clearCartridges() override
    {
        rack::CartridgeOrder blank;
        blank.fill(rack::noCartridge);
        publishCartridges(blank);
    }

    void toggleCartridge(int position) override
    {
        const auto order = cartridges();
        if (position < 0 || position >= slotCount(order) || order[position] < 0)
            return;
        auto& parameter = powerParameter(order[position]);
        cartridgeState.store(encodeCartridges(order), std::memory_order_release);
        setParameter(parameter, parameter.getValue() >= .5f ? 0.0f : 1.0f);
        updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
    }

    static void setParameter(juce::RangedAudioParameter& parameter, float normalized)
    {
        if (parameter.getValue() == normalized)
            return;
        parameter.beginChangeGesture();
        parameter.setValueNotifyingHost(normalized);
        parameter.endChangeGesture();
    }

    rack::DelayPreview delayPreview(int instance) const override
    {
        const auto layer = instance / moduleCount;
        const auto controls = delayControls({}, layer);
        const auto seconds = rack::Delay::repeatSeconds(controls, displayTempo.load(std::memory_order_relaxed));
        return { static_cast<float>(seconds * 1000.0),
                 controls.freeze ? 1.0f : std::min(controls.feedback, rack::Delay::maximumFeedback),
                 controls.mix, value(delayActive, layer) > .5f };
    }

    rack::GrainPicture grainPicture(int instance) const override
    {
        return banks[instance / moduleCount].grainEffect.picture();
    }

    rack::StretchPreview stretchPreview(int instance) const override
    {
        const auto layer = instance / moduleCount;
        const auto controls = stretchControls({}, layer);
        const auto& effect = banks[layer].stretchEffect;
        return { controls.ratio, controls.start, controls.land, effect.displayPhase(), effect.displayDebtSeconds(),
                 effect.displayRatio(), controls.shape, value(stretchActive, layer) > .5f, effect.displayStretching() };
    }

    std::array<float, 2> takeOutputPeaks() override
    {
        return { outputPeaks[0].exchange(0.0f, std::memory_order_relaxed),
                 outputPeaks[1].exchange(0.0f, std::memory_order_relaxed) };
    }

    void applyPreset(const std::vector<rack::PresetValue>& settings, rack::PresetIdentity identity)
    {
        orderEdit.fetch_add(1, std::memory_order_acq_rel);
        for (auto* item : getParameters())
        {
            auto* parameter = dynamic_cast<juce::RangedAudioParameter*>(item);
            if (parameter == nullptr)
                continue;
            const auto value = std::find_if(settings.begin(), settings.end(), [parameter](const auto& entry)
            {
                return parameter->paramID == entry.id;
            });
            setParameter(*parameter, value == settings.end() ? parameter->getDefaultValue()
                : parameter->convertTo0to1(value->value));
        }
        cartridgeState.store(0, std::memory_order_release);
        currentPreset.store(identity, std::memory_order_relaxed);
        orderEdit.fetch_add(1, std::memory_order_release);
        updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
    }

    void loadPreset(int index) override
    {
        if (index >= 0 && index < static_cast<int>(rack::presets().size()))
            applyPreset(rack::presets()[static_cast<size_t>(index)].values, { index });
    }

    bool loadPresetFile(const juce::File& file) override
    {
        const auto data = rack::readPresetComponent(file);
        if (!data)
            return false;
        orderEdit.fetch_add(1, std::memory_order_acq_rel);
        const auto loaded = restoreState(data->getData(), static_cast<int>(data->getSize()));
        orderEdit.fetch_add(1, std::memory_order_release);
        if (loaded)
            updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
        return loaded;
    }

    void generate(int tag, uint32_t seed) override
    {
        if (tag >= 0 && tag < static_cast<int>(rack::tagNames.size()))
        {
            const auto generated = rack::generatePreset(tag, seed);
            if (generated.variation >= 0)
                applyPreset(generated.values, { rack::generatedOriginBase + tag, generated.variation });
        }
    }

    rack::PresetIdentity presetIdentity() const override { return currentPreset.load(std::memory_order_relaxed); }

    void scrubTape(bool engaged, float speed) override
    {
        scrubSpeed.store(speed, std::memory_order_relaxed);
        scrubbing.store(engaged, std::memory_order_relaxed);
    }

    float tapeSpeed() const override { return transport.currentSpeed(); }

    float editorScale() const override { return savedEditorScale.load(std::memory_order_relaxed); }
    void setEditorScale(float scale) override
    {
        if (std::isfinite(scale))
            savedEditorScale.store(juce::jlimit(.8f, 1.5f, scale), std::memory_order_relaxed);
    }

    rack::ModulationActivity modulationActivity() const override { return modulation.activity(); }

    void renderChainFrame(std::array<float, 2>& frame, int channelCount, const std::array<rack::MotionFrames, rack::cartridgeCopies>& motions)
    {
        for (const auto instance : activeOrder)
        {
            if (instance < 0)
                break;
            const auto layer = instance / moduleCount;
            const auto effect = instance % moduleCount;
            auto& bank = banks[layer];
            const auto& motion = motions[layer];
            const auto wet = bank.moduleWet[effect].getNextValue() * entryLevel(bank.moduleEntry[effect]);
            switch (effect)
            {
                case tapeModule:
                {
                    auto effected = frame;
                    const auto& movement = motion[rack::motionTape];
                    bank.tapeEffect.process(effected, channelCount, 1.0f,
                                       .015f * movement.positiveDrift + .012f * std::abs(movement.unstable));
                    bank.tapeTexture.process(effected, channelCount);
                    rack::mixFrames(frame, effected, channelCount, wet);
                    break;
                }
                case tremoloModule:
                    bank.tremoloEffect.process(frame, channelCount, wet, .35f * motion[rack::motionTremolo].drift
                                                              + .5f * motion[rack::motionTremolo].unstable);
                    break;
                case saturationModule:
                    bank.saturationEffect.process(frame, channelCount, wet, .10f * motion[rack::motionSaturation].drift
                                                                  + .18f * motion[rack::motionSaturation].unstable);
                    break;
                case resamplerModule:
                    bank.resamplerEffect.process(frame, channelCount, wet, .06f * motion[rack::motionResampler].drift
                                                                 + .18f * motion[rack::motionResampler].unstable);
                    break;
                case chorusModule: bank.chorusEffect.process(frame, channelCount, wet); break;
                case delayModule:
                    bank.delayEffect.process(frame, channelCount, wet, .0126f * motion[rack::motionDelay].drift
                                                            + .009f * motion[rack::motionDelay].unstable);
                    break;
                case reverseModule: bank.reverseEffect.process(frame, channelCount, wet); break;
                case reverbModule:
                    bank.reverbEffect.process(frame, channelCount, wet, .0015f * motion[rack::motionReverb].drift
                                                             + .003f * motion[rack::motionReverb].unstable);
                    break;
                case stretchModule: bank.stretchEffect.process(frame, channelCount, wet); break;
                case vhsModule: bank.vhsEffect.process(frame, channelCount, wet); break;
                case grainModule: bank.grainEffect.process(frame, channelCount, wet); break;
                case moduleCount: jassertfalse; break;
            }
        }
    }

    float value(Parameter parameter, int layer = 0) const
    {
        return values[layer][parameter]->load(std::memory_order_relaxed);
    }
    float percent(Parameter parameter, int layer = 0) const { return value(parameter, layer) * 0.01f; }
    int option(Parameter parameter, int layer = 0) const
    {
        return static_cast<int>(std::lround(value(parameter, layer)));
    }

    static float route(float base, const rack::ModOffsets& offsets, rack::ModTarget target, float minimum, float maximum)
    {
        const auto amount = offsets[static_cast<size_t>(target)];
        return amount == 0 ? base : juce::jlimit(minimum, maximum, base + (maximum - minimum) * amount);
    }

    void applyRoutedTargets(const rack::ModOffsets& routed, int layer)
    {
        auto& bank = banks[layer];
        bank.tapeEffect.setTargets(tapeControls(routed, layer));
        bank.tremoloEffect.setRoutedTargets(tremoloControls(routed, layer));
        bank.saturationEffect.setTargets(saturationControls(routed, layer));
        bank.resamplerEffect.setTargets(resamplerControls(routed, layer));
        bank.chorusEffect.setTargets(chorusControls(routed, layer));
        bank.delayEffect.setRoutedTargets(delayControls(routed, layer));
        bank.reverseEffect.setRoutedTargets(reverseControls(routed, layer));
        bank.reverbEffect.setTargets(reverbControls(routed, layer));
        bank.stretchEffect.setRoutedTargets(stretchControls(routed, layer));
        bank.vhsEffect.setTargets(vhsControls(routed, layer));
        bank.grainEffect.setTargets(grainControls(routed, layer));
    }
    float globalPitchAmount() const { return value(globalPitch) + .01f * value(globalDetune); }

    rack::GlobalControls globalControls() const
    {
        return { value(inputGain), value(outputGain), percent(globalWidth), globalPitchAmount(), value(eqActive) > .5f,
                 { value(eqLowFrequency), value(eqMidFrequency), value(eqHighFrequency) },
                 { value(eqLowGain), value(eqMidGain), value(eqHighGain) } };
    }

    ChainOrder readOrder() const
    {
        ChainOrder result {};
        const auto& initial = legacyOrders[static_cast<size_t>(juce::jlimit(0, 5, option(effectOrder)))];
        std::copy(initial.begin(), initial.end(), result.begin());
        const auto insert = [&result](Module module, int size, int position)
        {
            const auto slot = juce::jlimit(0, size, position);
            std::copy_backward(result.begin() + slot, result.begin() + size, result.begin() + size + 1);
            result[static_cast<size_t>(slot)] = module;
        };
        insert(resamplerModule, 3, option(resamplerSlot));
        insert(chorusModule, 4, option(chorusSlot));
        insert(delayModule, 5, option(delaySlot));
        insert(reverseModule, 6, option(reverseSlot));
        insert(reverbModule, 7, option(reverbSlot));
        insert(stretchModule, 8, option(stretchSlot));
        // Sessions from before the VHS and Grain cartridges have no place for them; they run last.
        result[vhsModule] = vhsModule;
        result[grainModule] = grainModule;
        return result;
    }

    tape::Controls tapeControls(const rack::ModOffsets& m, int layer = 0) const
    {
        return { route(value(tapeDepth, layer), m, rack::tapeDepthTarget, 0, 100),
                 route(value(tapeWowRate, layer), m, rack::tapeWowTarget, .1f, 3),
                 route(value(tapeFlutterRate, layer), m, rack::tapeFlutterRateTarget, 3, 30),
                 route(value(tapeFlutterAmount, layer), m, rack::tapeFlutterAmountTarget, 0, 100),
                 value(tapeAgedColor, layer) };
    }

    rack::TremoloControls tremoloControls(const rack::ModOffsets& m, int layer = 0) const
    {
        return { route(percent(tremoloDepth, layer), m, rack::tremoloDepthTarget, 0, 1),
                 route(value(tremoloRate, layer), m, rack::tremoloRateTarget, .01f, 15),
                 route(percent(tremoloShape, layer), m, rack::tremoloShapeTarget, 0, 1),
                 juce::jlimit(0, 6, option(tremoloNote, layer)), juce::jlimit(0, 2, option(tremoloTiming, layer)),
                 value(tremoloSync, layer) > 0.5f, option(tremoloMode, layer) == 1, value(tremoloStereo, layer) > 0.5f };
    }

    rack::SaturationControls saturationControls(const rack::ModOffsets& m, int layer = 0) const
    {
        return { route(value(saturationDrive, layer), m, rack::saturationDriveTarget, 0, 30.9f),
                 route(percent(saturationTone, layer), m, rack::saturationToneTarget, 0, 1),
                 route(value(saturationFrequency, layer), m, rack::saturationFrequencyTarget, 50, 10000),
                 route(percent(saturationMix, layer), m, rack::saturationMixTarget, 0, 1),
                 option(saturationMode, layer) == 1 };
    }

    rack::TapeTextureControls textureControls(int layer = 0) const
    {
        return { value(tapeNoiseGain, layer), percent(tapeDropoutDepth, layer), value(tapeDropoutRate, layer),
                 juce::jlimit(0, 4, option(tapeNoiseType, layer)), value(tapeNoise, layer) > .5f };
    }

    rack::ResamplerControls resamplerControls(const rack::ModOffsets& m, int layer = 0) const
    {
        return { route(value(resamplerRate, layer), m, rack::resamplerRateTarget, 44, 44100),
                 route(value(resamplerDrive, layer), m, rack::resamplerDriveTarget, 0, 12),
                 route(percent(resamplerJitter, layer), m, rack::resamplerJitterTarget, 0, 1),
                 value(resamplerPreFilter, layer) > .5f, value(resamplerPostFilter, layer) > .5f,
                 modulation.changes(rack::resamplerRateTarget) };
    }

    rack::ChorusControls chorusControls(const rack::ModOffsets& m, int layer = 0) const
    {
        return { route(percent(chorusDepth, layer), m, rack::chorusDepthTarget, 0, 1),
                 route(value(chorusRate, layer), m, rack::chorusRateTarget, .01f, 5),
                 route(value(chorusSaturation, layer), m, rack::chorusSaturationTarget, 0, 18), percent(chorusAnalog, layer),
                 route(percent(chorusWidth, layer), m, rack::chorusWidthTarget, 0, 1),
                 route(percent(chorusMix, layer), m, rack::chorusMixTarget, 0, 1),
                 option(chorusWaveform, layer) == 1, value(chorusStereo, layer) > .5f };
    }

    rack::DelayControls delayControls(const rack::ModOffsets& m, int layer = 0) const
    {
        return { value(delayWarp, layer), value(delayTime, layer), route(percent(delayFeedback, layer), m, rack::delayFeedbackTarget, 0, 2),
                 route(value(delaySaturation, layer), m, rack::delaySaturationTarget, 0, 18),
                 route(percent(delayAnalog, layer), m, rack::delayAnalogTarget, 0, 1),
                 value(delayHighpass, layer), value(delayLowpass, layer), value(delayModRate, layer), percent(delayModDepth, layer),
                 route(percent(delayMix, layer), m, rack::delayMixTarget, 0, 1),
                 juce::jlimit(0, 6, option(delayNote, layer)), juce::jlimit(0, 2, option(delayTiming, layer)), option(delayType, layer) == 1,
                 value(delaySync, layer) > .5f, option(delaySlope, layer) == 1, value(delayFreeze, layer) > .5f };
    }

    rack::ReverseControls reverseControls(const rack::ModOffsets& m = {}, int layer = 0) const
    {
        return { route(percent(reverseForward, layer), m, rack::reverseForwardTarget, 0, 1),
                 route(value(reverseHighpass, layer), m, rack::reverseHighpassTarget, 10, 1000), value(reverseLowpass, layer),
                 route(value(reversePitch, layer), m, rack::reversePitchTarget, -12, 12),
                 route(percent(reverseMix, layer), m, rack::reverseMixTarget, 0, 1),
                 juce::jlimit(0, 6, option(reverseNote, layer)), juce::jlimit(0, 2, option(reverseTiming, layer)), option(reverseType, layer) == 1 };
    }

    rack::ReverbControls reverbControls(const rack::ModOffsets& m, int layer = 0) const
    {
        return { route(value(reverbPreDelay, layer), m, rack::reverbPreDelayTarget, 0, 200), percent(reverbEarlyLate, layer),
                 route(percent(reverbSize, layer), m, rack::reverbSizeTarget, 0, 1),
                 route(value(reverbDecay, layer), m, rack::reverbDecayTarget, .1f, 60), value(reverbHighpass, layer),
                 value(reverbLowpass, layer), value(reverbModRate, layer), percent(reverbModDepth, layer), value(reverbSaturation, layer),
                 route(percent(reverbMix, layer), m, rack::reverbMixTarget, 0, 1),
                 juce::jlimit(0, 2, option(reverbType, layer)) };
    }

    rack::VhsControls vhsControls(const rack::ModOffsets& m, int layer = 0) const
    {
        return { route(percent(vhsWear, layer), m, rack::vhsWearTarget, 0, 1),
                 route(percent(vhsTracking, layer), m, rack::vhsTrackingTarget, 0, 1),
                 route(percent(vhsWobble, layer), m, rack::vhsWobbleTarget, 0, 1),
                 route(percent(vhsMix, layer), m, rack::vhsMixTarget, 0, 1),
                 juce::jlimit(0, 2, option(vhsSpeed, layer)) };
    }

    rack::GrainControls grainControls(const rack::ModOffsets& m, int layer = 0) const
    {
        rack::GrainControls c;
        c.size = route(value(grainSize, layer), m, rack::grainSizeTarget, 10, 1000);
        c.density = route(value(grainDensity, layer), m, rack::grainDensityTarget, .5f, 100);
        c.position = route(value(grainPosition, layer), m, rack::grainPositionTarget, 0, 2000);
        c.pitch = route(value(grainPitch, layer), m, rack::grainPitchTarget, -24, 24);
        c.pitchRandom = route(percent(grainPitchRandom, layer), m, rack::grainPitchRandomTarget, 0, 1);
        c.sizeRandom = route(percent(grainSizeRandom, layer), m, rack::grainSizeRandomTarget, 0, 1);
        c.densityRandom = route(percent(grainDensityRandom, layer), m, rack::grainDensityRandomTarget, 0, 1);
        c.width = route(percent(grainWidth, layer), m, rack::grainWidthTarget, 0, 1);
        c.window = route(percent(grainWindow, layer), m, rack::grainWindowTarget, 0, 1);
        c.feedback = route(percent(grainFeedback, layer), m, rack::grainFeedbackTarget, 0, 1);
        c.highpass = value(grainHighpass, layer);
        c.lowpass = value(grainLowpass, layer);
        c.offset = route(value(grainOffset, layer), m, rack::grainOffsetTarget, 0, 250);
        c.toneHighpass = route(value(grainToneHighpass, layer), m, rack::grainToneHighpassTarget, 20, 5000);
        c.toneLowpass = route(value(grainToneLowpass, layer), m, rack::grainToneLowpassTarget, 200, 20000);
        c.stretchSpeed = route(percent(grainStretchSpeed, layer), m, rack::grainStretchSpeedTarget, 0, 1);
        c.mix = route(percent(grainMix, layer), m, rack::grainMixTarget, 0, 1);
        c.scale = juce::jlimit(0, 16, option(grainScale, layer));
        c.voices = juce::jlimit(1, 16, static_cast<int>(std::lround(route(value(grainVoices, layer), m, rack::grainVoicesTarget, 1, 16))));
        c.division = juce::jlimit(0, 25, option(grainDivision, layer));
        c.retrigger = juce::jlimit(0, 26, option(grainRetrigger, layer));
        c.reverse = value(grainReverse, layer) > .5f;
        c.sync = value(grainSync, layer) > .5f;
        c.stretch = value(grainStretch, layer) > .5f;
        return c;
    }

    rack::StretchControls stretchControls(const rack::ModOffsets& m, int layer = 0) const
    {
        return { route(value(stretchRatio, layer), m, rack::stretchRatioTarget, 1, 20), percent(stretchStart, layer),
                 percent(stretchLand, layer), route(value(stretchCycle, layer), m, rack::stretchCycleTarget, 20, 4000),
                 percent(stretchOverlap, layer), route(percent(stretchMix, layer), m, rack::stretchMixTarget, 0, 1),
                 juce::jlimit(0, 4, option(stretchLength, layer)), juce::jlimit(0, 4, option(stretchShape, layer)),
                 juce::jlimit(0, 3, option(stretchEngine, layer)) };
    }

    juce::AudioProcessorValueTreeState state;
    juce::ValueTree defaultState;
    std::atomic<unsigned> orderEdit { 0 };
    std::atomic<rack::PresetIdentity> currentPreset { rack::PresetIdentity{} };
    std::atomic<float> savedEditorScale { 1.f };
    std::atomic<float> displayTempo { 120.0f };
    std::array<std::atomic<float>, 2> outputPeaks {};
    rack::ModulationParameters modulationParameters;
    rack::Modulation modulation;
    rack::GlobalEffects globals;
    juce::AudioBuffer<float> wetBuffer;
    std::vector<float> blendBuffer;
    std::array<std::array<const std::atomic<float>*, parameterCount>, rack::cartridgeCopies> values {};
    std::array<EffectBank, rack::cartridgeCopies> banks;
    std::atomic<uint64_t> cartridgeState { 0 };
    juce::SmoothedValue<float> mix, orderFade;
    // Instances whose cartridge is being pulled out of the rack, one bit each.
    std::atomic<uint32_t> leavingCartridges { 0 };

    // Starts `entry` towards `target` from wherever it is now, taking `seconds` for the whole way.
    void rampEntry(juce::SmoothedValue<float>& entry, float target, double seconds)
    {
        const auto from = entry.getCurrentValue();
        entry.reset(getSampleRate(), seconds);
        entry.setCurrentAndTargetValue(from);
        entry.setTargetValue(target);
    }
    rack::TapeTransport transport;
    rack::GenerationLoss generationLoss;
    std::atomic<bool> scrubbing { false };
    std::atomic<float> scrubSpeed { 0.0f };
    int heldNotes = 0;
    rack::CartridgeOrder activeOrder {};
};
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RackProcessor();
}
