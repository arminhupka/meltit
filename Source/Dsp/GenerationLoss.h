#pragma once

#include "Effects.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cmath>
#include <vector>

namespace rack
{
// The rack's sound copied tape to tape, `generations` times (1 is the original). Each copy loses a little top end
// and bottom end, adds its own hiss on top of the hiss it was given, saturates a touch more and adds its machine's
// wow and flutter to the ones already printed. One setting runs from a fresh master to a tenth-generation dub.
class GenerationLoss
{
public:
    void prepare(double rate, float generations)
    {
        sampleRate = rate;
        const auto length = static_cast<size_t>(std::ceil(rate * .012)) + 4;
        for (auto& line : lines)
            line.assign(length, 0.0f);
        write = 0;
        initialise(copies, rate, copiesFor(generations), .08);
        wowPhase = flutterPhase = 0;
        drift = driftTarget = 0;
        highState.fill(0.0f);
        for (auto& channel : lowpass)
            for (auto& filter : channel)
                filter = {};
        countdown = 0;
        updateFilters(copies.getCurrentValue());
    }

    void setTarget(float generations) { copies.setTargetValue(copiesFor(generations)); }

    void process(std::array<float, 2>& samples, int channelCount)
    {
        const auto g = copies.getNextValue();
        const auto size = lines[0].size();
        for (int c = 0; c < 2; ++c)
            lines[static_cast<size_t>(c)][write] = samples[static_cast<size_t>(juce::jmin(c, channelCount - 1))];
        const auto writtenAt = write;
        write = (write + 1) % size;
        if (g <= .0005f)
            return;
        if (++countdown >= 32)
        {
            countdown = 0;
            updateFilters(g);
        }

        // Every machine in the chain adds its own wow and flutter; independent wobbles grow with the square root.
        const auto twoPi = juce::MathConstants<double>::twoPi;
        wowPhase += .48 / sampleRate;
        flutterPhase += 7.1 / sampleRate;
        wowPhase -= std::floor(wowPhase);
        flutterPhase -= std::floor(flutterPhase);
        if (--driftCountdown <= 0)
        {
            driftCountdown = static_cast<int>(sampleRate * .4);
            driftTarget = random.nextFloat() * 2.0f - 1.0f;
        }
        drift += (driftTarget - drift) * static_cast<float>(2.0 / sampleRate);
        const auto spread = std::sqrt(static_cast<double>(g));
        // The delay only ever grows from a couple of samples, so a light setting mixed with the clean signal
        // doesn't comb-filter it.
        const auto wobble = spread * (.00022 * (1.0 + std::sin(twoPi * wowPhase)) + .00009 * (1.0 + drift)
                                      + .00004 * (1.0 + std::sin(twoPi * flutterPhase)));
        const auto delay = juce::jlimit(2.0, static_cast<double>(size) - 3.0, 2.0 + wobble * sampleRate);
        auto read = static_cast<double>(writtenAt) - delay;
        if (read < 0)
            read += static_cast<double>(size);

        // Fade in from the clean signal over the first copy, so 1.0 stays untouched and the path has no jump.
        const auto blend = juce::jmin(1.0f, g);
        const auto drive = 1.0f + .12f * g;
        const auto hiss = juce::Decibels::decibelsToGain(-84.0f + 10.0f * std::log10(juce::jmax(1.0f, g)) + 2.6f * g);
        for (int c = 0; c < channelCount; ++c)
        {
            const auto index = static_cast<size_t>(c);
            auto sample = readLine(lines[index], read);
            // Earlier copies' hiss goes through every later copy's filters too.
            sample += hiss * (random.nextFloat() * 2.0f - 1.0f);
            sample = std::tanh(drive * sample) / drive;
            highState[index] += highCoefficient * (sample - highState[index]);
            sample -= highState[index];
            for (auto& filter : lowpass[index])
                sample = filter.process(sample);
            samples[index] = std::lerp(samples[index], sample, blend);
        }
    }

private:
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

        float process(float input)
        {
            const auto output = b0 * input + z1;
            z1 = b1 * input - a1 * output + z2;
            z2 = b2 * input - a2 * output;
            return output;
        }
    };

    static float copiesFor(float generations) { return juce::jlimit(0.0f, 9.0f, generations - 1.0f); }

    void updateFilters(float g)
    {
        // Each copy keeps about 84 percent of the band above it, and a little less of the lows.
        const auto corner = juce::jlimit(1500.0f, static_cast<float>(sampleRate * .45), 19000.0f * std::pow(.84f, g));
        const auto k = std::tan(juce::MathConstants<float>::pi * corner / static_cast<float>(sampleRate));
        constexpr std::array q { .5412f, 1.3066f };
        for (size_t stage = 0; stage < q.size(); ++stage)
        {
            const auto norm = 1.0f / (1.0f + k / q[stage] + k * k);
            for (auto& channel : lowpass)
            {
                auto& filter = channel[stage];
                filter.b0 = k * k * norm;
                filter.b1 = 2.0f * filter.b0;
                filter.b2 = filter.b0;
                filter.a1 = 2.0f * (k * k - 1.0f) * norm;
                filter.a2 = (1.0f - k / q[stage] + k * k) * norm;
            }
        }
        const auto bass = 18.0f * std::pow(1.22f, g);
        highCoefficient = static_cast<float>(1.0 - std::exp(-juce::MathConstants<double>::twoPi * bass / sampleRate));
    }

    static float readLine(const std::vector<float>& line, double position)
    {
        const auto size = line.size();
        const auto index = static_cast<size_t>(position);
        const auto t = static_cast<float>(position - static_cast<double>(index));
        const auto at = [&line, size](size_t i) { return line[i % size]; };
        const auto y0 = at(index + size - 1), y1 = at(index), y2 = at(index + 1), y3 = at(index + 2);
        const auto c1 = .5f * (y2 - y0);
        const auto c2 = y0 - 2.5f * y1 + 2.0f * y2 - .5f * y3;
        const auto c3 = .5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }

    double sampleRate = 48000;
    std::array<std::vector<float>, 2> lines;
    size_t write = 0;
    juce::SmoothedValue<float> copies;
    std::array<std::array<Biquad, 2>, 2> lowpass;
    std::array<float, 2> highState {};
    float highCoefficient = 0, drift = 0, driftTarget = 0;
    int countdown = 0, driftCountdown = 0;
    double wowPhase = 0, flutterPhase = 0;
    juce::Random random;
};
}
