#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace rack
{
inline void initialise(juce::SmoothedValue<float>& parameter, double rate, float value, double seconds = 0.01)
{
    parameter.reset(rate, seconds);
    parameter.setCurrentAndTargetValue(value);
}

inline double beatsForDivision(int note, int timing)
{
    constexpr std::array noteBeats { .0625, .125, .25, .5, 1.0, 2.0, 4.0 };
    constexpr std::array subdivisions { 1.0, 2.0 / 3.0, 1.5 };
    return noteBeats[static_cast<size_t>(note)] * subdivisions[static_cast<size_t>(timing)];
}

struct TremoloControls
{
    float depth, rate, shape;
    int note, timing;
    bool sync, hard, stereo;
};

class Tremolo
{
public:
    void prepare(double rate, TremoloControls controls)
    {
        sampleRate = rate;
        phase = 0.0;
        tempo = 120.0;
        initialise(depth, rate, controls.depth);
        initialise(shape, rate, controls.shape);
        initialise(hard, rate, static_cast<float>(controls.hard));
        initialise(stereo, rate, static_cast<float>(controls.stereo));
        gainCoefficient = static_cast<float>(1.0 - std::exp(-1.0 / (rate * 0.0005)));
        previousGain = { 1.0f, 1.0f };
    }

    void beginBlock(TremoloControls controls, const juce::AudioPlayHead* playHead)
    {
        synced = controls.sync;
        setRoutedTargets(controls);
        shape.setTargetValue(controls.shape);
        hard.setTargetValue(static_cast<float>(controls.hard));
        stereo.setTargetValue(static_cast<float>(controls.stereo));
        step = controls.rate / sampleRate;
        if (!controls.sync)
            return;

        const auto beats = beatsForDivision(controls.note, controls.timing);
        if (playHead != nullptr)
        {
            if (const auto position = playHead->getPosition())
            {
                if (const auto bpm = position->getBpm(); bpm && std::isfinite(*bpm) && *bpm > 0)
                    tempo = *bpm;
                const auto ppq = position->getPpqPosition();
                if (position->getIsPlaying() && ppq && std::isfinite(*ppq))
                {
                    phase = *ppq / beats;
                    phase -= std::floor(phase);
                }
            }
        }
        step = tempo / (60.0 * beats * sampleRate);
    }

    void setRoutedTargets(TremoloControls controls)
    {
        depth.setTargetValue(controls.depth);
        if (!synced)
            step = controls.rate / sampleRate;
    }

    void process(std::array<float, 2>& samples, int channelCount, float wet, float motion = 0)
    {
        const auto peak = std::lerp(0.25, 0.75, static_cast<double>(shape.getNextValue()));
        auto position = phase;
        if (synced && motion != 0)
        {
            position += .5 * motion;
            position -= std::floor(position);
        }
        const auto triangle = static_cast<float>(position < peak ? position / peak : (1.0 - position) / (1.0 - peak));
        const auto curved = 0.5f + 0.5f * std::tanh(2.55f * (2.0f * triangle - 1.0f)) / 0.9878804f;
        const auto envelope = std::lerp(triangle, curved, hard.getNextValue());
        const auto rightEnvelope = std::lerp(envelope, 1.0f - envelope, stereo.getNextValue());
        const auto amount = depth.getNextValue();
        for (int channel = 0; channel < channelCount; ++channel)
        {
            const auto index = static_cast<size_t>(channel);
            const auto target = std::lerp(1.0f, channel == 0 ? envelope : rightEnvelope, amount);
            previousGain[index] += gainCoefficient * (target - previousGain[index]);
            samples[index] *= std::lerp(1.0f, previousGain[index], wet);
        }
        phase += synced ? step : step * (1.0 + motion);
        phase -= std::floor(phase);
    }

private:
    juce::SmoothedValue<float> depth, shape, hard, stereo;
    std::array<float, 2> previousGain { 1.0f, 1.0f };
    double sampleRate = 48000;
    double phase = 0;
    double step = 0;
    double tempo = 120;
    float gainCoefficient = 1;
    bool synced = false;
};

struct SaturationControls
{
    float drive, tone, frequency, mix;
    bool saturate;
};

class Saturation
{
public:
    void prepare(double rate, SaturationControls controls)
    {
        sampleRate = rate;
        lowState.fill(0.0f);
        initialise(gain, rate, driveGain(controls.drive));
        initialise(softTrim, rate, trim(controls.drive, softTrimDb));
        initialise(hardTrim, rate, trim(controls.drive, hardTrimDb));
        initialise(driveMix, rate, driveBlend(controls.drive));
        initialise(tone, rate, controls.tone);
        initialise(frequency, rate, filterCoefficient(controls.frequency));
        initialise(mix, rate, controls.mix);
        initialise(mode, rate, static_cast<float>(controls.saturate));
    }

    void setTargets(SaturationControls controls)
    {
        gain.setTargetValue(driveGain(controls.drive));
        softTrim.setTargetValue(trim(controls.drive, softTrimDb));
        hardTrim.setTargetValue(trim(controls.drive, hardTrimDb));
        driveMix.setTargetValue(driveBlend(controls.drive));
        tone.setTargetValue(controls.tone);
        frequency.setTargetValue(filterCoefficient(controls.frequency));
        mix.setTargetValue(controls.mix);
        mode.setTargetValue(static_cast<float>(controls.saturate));
    }

    void process(std::array<float, 2>& samples, int channelCount, float wet, float motion = 0)
    {
        const auto multiplier = gain.getNextValue() * (1.0f + motion);
        const auto softLevel = softTrim.getNextValue();
        const auto hardLevel = hardTrim.getNextValue();
        const auto modeBlend = mode.getNextValue();
        const auto amount = wet * mix.getNextValue() * driveMix.getNextValue();
        const auto balance = (tone.getNextValue() - 0.5f) * 2.0f;
        const auto coefficient = frequency.getNextValue();
        const auto shapeSample = [=] (float sample)
        {
            const auto input = multiplier * sample;
            const auto soft = std::copysign(-std::expm1(-std::abs(input)), input) * softLevel;
            const auto saturated = input / (1.0f + input * input) * hardLevel;
            return std::lerp(soft, saturated, modeBlend);
        };
        for (int channel = 0; channel < channelCount; ++channel)
        {
            const auto index = static_cast<size_t>(channel);
            auto& sample = samples[index];
            auto& low = lowState[index];
            low += coefficient * (sample - low);
            if (amount == 0.0f)
                continue;
            const auto full = shapeSample(sample);
            const auto band = balance < 0.0f ? low : sample - low;
            const auto selective = shapeSample(band) + sample - band;
            sample = std::lerp(sample, std::lerp(full, selective, std::abs(balance)), amount);
        }
    }

private:
    static float driveGain(float db) { return juce::Decibels::decibelsToGain(db); }
    static float driveBlend(float db) { return std::min(1.0f, db / 3.7f); }

    float filterCoefficient(float hertz) const
    {
        return static_cast<float>(1.0 - std::exp(-juce::MathConstants<double>::twoPi * hertz / sampleRate));
    }

    // Compensation measured from the fundamental at neutral Tone.
    static constexpr std::array driveDb { 0.0f, 3.7f, 8.9f, 14.4f, 23.7f, 30.9f };
    static constexpr std::array softTrimDb { 0.0f, -1.65f, -3.75f, -5.55f, -7.45f, -8.22f };
    static constexpr std::array hardTrimDb { 0.0f, -2.19f, -4.25f, -6.01f, -7.88f, -8.59f };

    static float trim(float drive, const std::array<float, driveDb.size()>& values)
    {
        const auto upper = std::upper_bound(driveDb.begin(), driveDb.end(), drive);
        if (upper == driveDb.end())
            return driveGain(values.back());
        const auto index = static_cast<size_t>(std::distance(driveDb.begin(), upper));
        const auto fraction = (drive - driveDb[index - 1]) / (driveDb[index] - driveDb[index - 1]);
        return driveGain(std::lerp(values[index - 1], values[index], fraction));
    }

    juce::SmoothedValue<float> gain, softTrim, hardTrim, driveMix, tone, frequency, mix, mode;
    std::array<float, 2> lowState {};
    double sampleRate = 48000;
};
}
