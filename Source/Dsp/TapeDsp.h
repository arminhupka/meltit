#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cmath>
#include <vector>

namespace tape
{
constexpr auto twoPi = juce::MathConstants<double>::twoPi;
// Full Depth with Flutter at its widest reaches about 0.145 s; the analog motion pedals can add up to 0.084 s.
constexpr auto maximumDelaySeconds = 0.25;
constexpr auto parameterSmoothingSeconds = 0.04;

using Filter = juce::dsp::IIR::Filter<float>;
using Coefficients = juce::dsp::IIR::Coefficients<float>;

struct Channel
{
    void prepare(double sampleRate, int delayCapacity)
    {
        delay.assign(static_cast<size_t>(delayCapacity), 0.0f);
        for (auto& filter : highpass)
            filter.coefficients = Coefficients::makeFirstOrderHighPass(sampleRate, 20.0f);
        for (auto& filter : lowpass)
            filter.coefficients = Coefficients::makeFirstOrderLowPass(
                sampleRate, static_cast<float>(juce::jmin(22000.0, sampleRate * 0.48)));
        bass.coefficients = Coefficients::makeLowShelf(sampleRate, 175.0f, 0.71f,
                                                       juce::Decibels::decibelsToGain(-4.2f));
        presence.coefficients = Coefficients::makePeakFilter(sampleRate, 2550.0f, 0.85f,
                                                             juce::Decibels::decibelsToGain(5.3f));
        treble.coefficients = Coefficients::makeLowPass(
            sampleRate, static_cast<float>(juce::jmin(6250.0, sampleRate * 0.40)), 0.73f);

        for (auto& filter : highpass)
            filter.reset();
        for (auto& filter : lowpass)
            filter.reset();
        bass.reset();
        presence.reset();
        treble.reset();
    }

    float process(float input, int writeIndex, float delaySamples, float aged)
    {
        const auto capacity = static_cast<int>(delay.size());
        const auto whole = static_cast<int>(delaySamples);
        const auto fraction = delaySamples - static_cast<float>(whole);
        const auto first = writeIndex >= whole ? writeIndex - whole : writeIndex - whole + capacity;
        const auto second = first > 0 ? first - 1 : capacity - 1;
        delay[static_cast<size_t>(writeIndex)] = input;
        auto clean = std::lerp(delay[static_cast<size_t>(first)], delay[static_cast<size_t>(second)], fraction);
        for (auto& filter : highpass)
            clean = filter.processSample(clean);
        for (auto& filter : lowpass)
            clean = filter.processSample(clean);
        const auto coloured = treble.processSample(presence.processSample(bass.processSample(clean)));
        return std::lerp(clean, coloured, aged);
    }

    std::vector<float> delay;
    std::array<Filter, 2> highpass;
    std::array<Filter, 2> lowpass;
    Filter bass, presence, treble;
};

struct Controls
{
    float depth;
    float wowRate;
    float flutterRate;
    float flutterAmount;
    float age;
};

class Processor
{
public:
    void prepare(double newSampleRate, Controls controls)
    {
        sampleRate = newSampleRate;
        capacity = static_cast<int>(std::ceil(newSampleRate * maximumDelaySeconds)) + 2;
        for (auto& channel : channels)
            channel.prepare(newSampleRate, capacity);

        writeIndex = 0;
        wowPhase = 0.0;
        flutterPhase = 0.0;
        random.setSeed(juce::Random::getSystemRandom().nextInt64());
        for (auto& knot : flutterKnots)
            knot = nextFlutterKnot();

        resetSmoother(depth, controls.depth * 0.1f);
        resetSmoother(wowRate, controls.wowRate);
        resetSmoother(flutterRate, controls.flutterRate);
        resetSmoother(flutter, controls.flutterAmount * 0.01f);
        resetSmoother(age, controls.age * 0.01f);
    }

    void setTargets(Controls controls)
    {
        depth.setTargetValue(controls.depth * 0.1f);
        wowRate.setTargetValue(controls.wowRate);
        flutterRate.setTargetValue(controls.flutterRate);
        flutter.setTargetValue(controls.flutterAmount * 0.01f);
        age.setTargetValue(controls.age * 0.01f);
    }

    void process(std::array<float, 2>& samples, int numChannels, float wet, float motionSeconds = 0)
    {
        const auto delaySamples = nextDelaySamples(motionSeconds);
        const auto aged = age.getNextValue();
        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto& sample = samples[static_cast<size_t>(channel)];
            const auto processed = channels[static_cast<size_t>(channel)].process(sample, writeIndex, delaySamples, aged);
            sample = std::lerp(sample, processed, wet);
        }
        if (++writeIndex == capacity)
            writeIndex = 0;
    }

private:
    void resetSmoother(juce::SmoothedValue<float>& smoother, float value) const
    {
        smoother.reset(sampleRate, parameterSmoothingSeconds);
        smoother.setCurrentAndTargetValue(value);
    }

    float nextFlutterKnot() { return 2.0f * random.nextFloat() - 1.0f; }

    float nextDelaySamples(float motionSeconds)
    {
        const auto phase = twoPi * wowPhase;
        const auto cosine = std::cos(phase);
        // Harmonics of the delay trajectory measured at 10% Depth and zero Flutter.
        const auto wow = cosine * (0.002941 - 3.0 * 0.000058 + 4.0 * 0.000058 * cosine * cosine)
                       + 0.000498 * (2.0 * cosine * cosine - 1.0);
        wowPhase += wowRate.getNextValue() / sampleRate;
        if (wowPhase >= 1.0)
            wowPhase -= 1.0;

        const auto t = static_cast<float>(flutterPhase);
        const auto [a, b, c, d] = flutterKnots;
        const auto flutterPosition = b + 0.5f * t * ((c - a) + t * (
            2.0f * a - 5.0f * b + 4.0f * c - d + t * (3.0f * (b - c) + d - a)));
        flutterPhase += flutterRate.getNextValue() / sampleRate;
        if (flutterPhase >= 1.0)
        {
            flutterPhase -= 1.0;
            flutterKnots = { b, c, d, nextFlutterKnot() };
        }

        const auto amount = flutter.getNextValue();
        const auto wowGain = juce::jmin(1.0f, 2.0f * (1.0f - amount));
        const auto flutterGain = amount <= 0.5f ? 2.0f * amount : 1.0f - 0.3f * (amount - 0.5f);
        const auto delaySeconds = depth.getNextValue() * (
            wowGain * (0.0035 + wow) + flutterGain * (0.0041 + 0.0032 * flutterPosition));
        return juce::jlimit(0.0f, static_cast<float>(capacity - 2), static_cast<float>((delaySeconds + motionSeconds) * sampleRate));
    }

    juce::SmoothedValue<float> depth, wowRate, flutterRate, flutter, age;
    juce::Random random;
    std::array<float, 4> flutterKnots {};
    std::array<Channel, 2> channels;
    double sampleRate = 48000.0;
    double wowPhase = 0.0;
    double flutterPhase = 0.0;
    int capacity = 0;
    int writeIndex = 0;
};
}
