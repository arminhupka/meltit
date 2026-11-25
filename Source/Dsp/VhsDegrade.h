#pragma once

#include "Effects.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cmath>
#include <vector>

namespace rack
{
struct VhsControls
{
    float wear, tracking, wobble, mix;
    int speed; // 0 SP, 1 LP, 2 EP: the slower the tape, the narrower, noisier and wobblier it sounds.
};

// The audio of a worn VHS tape: the linear track's narrow band and roll-off, hiss, wow and flutter from a
// stretched tape, dropouts from worn oxide and tracking errors, where the heads lose the track for a moment and
// the sound dips, buzzes and jitters before it locks again.
class VhsDegrade
{
public:
    void prepare(double rate, VhsControls controls)
    {
        sampleRate = rate;
        const auto length = static_cast<size_t>(std::ceil(rate * .04)) + 4;
        for (auto& line : lines)
            line.assign(length, 0.0f);
        write = 0;
        initialise(wear, rate, controls.wear, .05);
        initialise(tracking, rate, controls.tracking, .05);
        initialise(wobble, rate, controls.wobble, .05);
        initialise(mix, rate, controls.mix);
        initialise(cutoff, rate, cutoffFor(controls), .08);
        initialise(speedScale, rate, speedFactor(controls.speed), .2);
        initialise(lowCut, rate, lowCutFor(controls.speed), .2);
        wowPhase = flutterPhase = buzzPhase = 0;
        drift = driftTarget = jitter = jitterTarget = 0;
        hissState.fill(0.0f);
        highState.fill(0.0f);
        for (auto& stage : lowpass)
            for (auto& filter : stage)
                filter.reset();
        dropout = {};
        trackingError = {};
        updateFilters(cutoff.getCurrentValue(), lowCut.getCurrentValue());
    }

    void setTargets(VhsControls controls)
    {
        wear.setTargetValue(controls.wear);
        tracking.setTargetValue(controls.tracking);
        wobble.setTargetValue(controls.wobble);
        mix.setTargetValue(controls.mix);
        cutoff.setTargetValue(cutoffFor(controls));
        speedScale.setTargetValue(speedFactor(controls.speed));
        lowCut.setTargetValue(lowCutFor(controls.speed));
    }

    void process(std::array<float, 2>& samples, int channelCount, float wet)
    {
        const auto worn = wear.getNextValue();
        const auto lost = tracking.getNextValue();
        const auto sway = wobble.getNextValue();
        const auto slow = speedScale.getNextValue();
        const auto amount = wet * mix.getNextValue();
        const auto corner = cutoff.getNextValue();
        const auto bass = lowCut.getNextValue();
        if (++filterCountdown >= 16)
        {
            filterCountdown = 0;
            updateFilters(corner, bass);
        }

        // Write the incoming audio before reading, so the wet path always has history behind it.
        const auto size = lines[0].size();
        for (int channel = 0; channel < 2; ++channel)
            lines[static_cast<size_t>(channel)][write] = samples[static_cast<size_t>(juce::jmin(channel, channelCount - 1))];

        const auto events = advanceEvents(worn, lost);
        const auto delaySeconds = transportDelay(sway, slow, lost, events.tracking);
        // At least two samples back, so the four-point read never reaches past the sample just written.
        const auto delay = juce::jlimit(2.0, static_cast<double>(size) - 3.0, delaySeconds * sampleRate);
        auto read = static_cast<double>(write) - delay;
        if (read < 0)
            read += static_cast<double>(size);
        write = (write + 1) % size;

        const auto drive = 1.0f + 2.5f * worn;
        const auto hissLevel = juce::Decibels::decibelsToGain(-74.0f + 30.0f * worn + 5.0f * (slow - 1.0f))
                             * (1.0f + 5.0f * events.tracking);
        const auto buzz = headSwitch(lost, events.tracking);
        const auto level = (1.0f - .85f * events.dropout) * (1.0f - .55f * events.tracking * (.5f + .5f * events.flicker));

        for (int channel = 0; channel < channelCount; ++channel)
        {
            const auto index = static_cast<size_t>(channel);
            auto sample = readLine(lines[index], read);
            sample = std::tanh(drive * sample) / drive * (1.0f + .25f * worn);
            highState[index] += highCoefficient * (sample - highState[index]);
            sample -= highState[index];
            for (auto& filter : lowpass[index])
                sample = filter.process(sample);
            sample *= level;
            // Hiss: white noise softened a little, a different grain in each channel.
            hissState[index] += .45f * ((random.nextFloat() * 2.0f - 1.0f) - hissState[index]);
            sample += hissState[index] * hissLevel + buzz;
            samples[index] = std::lerp(samples[index], sample, amount);
        }
    }

private:
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

        void reset() { z1 = z2 = 0; }

        float process(float input)
        {
            const auto output = b0 * input + z1;
            z1 = b1 * input - a1 * output + z2;
            z2 = b2 * input - a2 * output;
            return output;
        }
    };

    struct Event
    {
        double remaining = 0, length = 1;
        float depth = 0;
    };

    struct Levels
    {
        float dropout, tracking, flicker;
    };

    static float speedFactor(int speed) { return std::array { 1.0f, 1.6f, 2.4f }[static_cast<size_t>(juce::jlimit(0, 2, speed))]; }
    static float lowCutFor(int speed) { return std::array { 60.0f, 85.0f, 110.0f }[static_cast<size_t>(juce::jlimit(0, 2, speed))]; }

    static float cutoffFor(const VhsControls& controls)
    {
        const auto band = std::array { 12000.0f, 7500.0f, 4800.0f }[static_cast<size_t>(juce::jlimit(0, 2, controls.speed))];
        return band * (1.0f - .45f * controls.wear);
    }

    void updateFilters(float corner, float bass)
    {
        const auto frequency = juce::jlimit(200.0f, static_cast<float>(sampleRate * .45), corner);
        // Two Butterworth sections: a steep 24 dB roll-off like the linear audio track.
        constexpr std::array q { .5412f, 1.3066f };
        const auto k = std::tan(juce::MathConstants<float>::pi * frequency / static_cast<float>(sampleRate));
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
        highCoefficient = static_cast<float>(1.0 - std::exp(-juce::MathConstants<double>::twoPi * bass / sampleRate));
    }

    // Starts and runs the dropout and tracking events; returns how deep each one is right now.
    Levels advanceEvents(float worn, float lost)
    {
        const auto step = 1.0 / sampleRate;
        // Worn oxide drops out a few times a second at most; a badly tracked tape loses the track every second or so.
        if (dropout.remaining <= 0 && random.nextDouble() < worn * worn * 1.6 * step)
            start(dropout, .02 + .1 * random.nextDouble(), (.3f + .55f * random.nextFloat()) * worn);
        if (trackingError.remaining <= 0 && random.nextDouble() < lost * lost * 1.4 * step)
            start(trackingError, .06 + .3 * random.nextDouble(), .4f + .6f * lost);
        const auto shape = [step](Event& event)
        {
            if (event.remaining <= 0)
                return 0.0f;
            event.remaining -= step;
            const auto u = static_cast<float>(1.0 - event.remaining / event.length);
            return event.depth * std::sin(juce::MathConstants<float>::pi * juce::jlimit(0.0f, 1.0f, u));
        };
        // While the track is lost the level stutters between two states, as the picture flickers.
        if (--flickerCountdown <= 0)
        {
            flickerCountdown = static_cast<int>(sampleRate * (.008 + .02 * random.nextDouble()));
            flicker = random.nextFloat();
        }
        return { shape(dropout), shape(trackingError), flicker };
    }

    void start(Event& event, double seconds, float depth)
    {
        event.length = event.remaining = seconds;
        event.depth = juce::jlimit(0.0f, 1.0f, depth);
    }

    // How long the tape path delays the audio now, in seconds: a fixed run plus wow, flutter, a slow drift and,
    // while tracking is lost, a nervous jitter.
    double transportDelay(float sway, float slow, float lost, float error)
    {
        const auto twoPi = juce::MathConstants<double>::twoPi;
        wowPhase += .62 / sampleRate;
        flutterPhase += 9.3 / sampleRate;
        wowPhase -= std::floor(wowPhase);
        flutterPhase -= std::floor(flutterPhase);
        if (--driftCountdown <= 0)
        {
            driftCountdown = static_cast<int>(sampleRate * .25);
            driftTarget = random.nextFloat() * 2.0f - 1.0f;
            jitterTarget = random.nextFloat() * 2.0f - 1.0f;
        }
        drift += (driftTarget - drift) * static_cast<float>(1.5 / sampleRate);
        jitter += (jitterTarget - jitter) * static_cast<float>(60.0 / sampleRate);
        const auto wow = .0025 * std::sin(twoPi * wowPhase) + .0008 * drift;
        const auto flutter = .00015 * std::sin(twoPi * flutterPhase);
        const auto unstable = .0006 * jitter * (error + .15f * lost);
        // The fixed run grows with the swing a slower tape gets, so full wobble at LP or EP never pulls the delay
        // below zero: the widest swing is 3.45 ms times `slow`, plus 0.7 ms of jitter.
        const auto run = .0045 * static_cast<double>(slow);
        return run + static_cast<double>(sway * slow) * (wow + flutter) + unstable;
    }

    // The buzz of the video heads switching, 59.94 times a second: faint on a badly tracked tape, loud while the
    // track is lost.
    float headSwitch(float lost, float error)
    {
        buzzPhase += 59.94 / sampleRate;
        buzzPhase -= std::floor(buzzPhase);
        const auto pulse = buzzPhase < .04 ? 1.0f - static_cast<float>(buzzPhase / .04) : 0.0f;
        return pulse * juce::Decibels::decibelsToGain(-58.0f) * (lost * lost + 6.0f * error);
    }

    static float readLine(const std::vector<float>& line, double position)
    {
        const auto size = line.size();
        const auto index = static_cast<size_t>(position);
        const auto t = static_cast<float>(position - static_cast<double>(index));
        const auto at = [&line, size](size_t i) { return line[i % size]; };
        const auto y0 = at(index + size - 1), y1 = at(index), y2 = at(index + 1), y3 = at(index + 2);
        // Four-point Hermite, smooth enough for a slowly swaying delay.
        const auto c1 = .5f * (y2 - y0);
        const auto c2 = y0 - 2.5f * y1 + 2.0f * y2 - .5f * y3;
        const auto c3 = .5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }

    double sampleRate = 48000;
    std::array<std::vector<float>, 2> lines;
    size_t write = 0;
    juce::SmoothedValue<float> wear, tracking, wobble, mix, cutoff, speedScale, lowCut;
    std::array<std::array<Biquad, 2>, 2> lowpass;
    std::array<float, 2> hissState {}, highState {};
    float highCoefficient = 0;
    int filterCountdown = 0, flickerCountdown = 0, driftCountdown = 0;
    double wowPhase = 0, flutterPhase = 0, buzzPhase = 0;
    float drift = 0, driftTarget = 0, jitter = 0, jitterTarget = 0, flicker = 0;
    Event dropout, trackingError;
    juce::Random random;
};
}
