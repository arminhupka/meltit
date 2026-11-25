#pragma once

#include "SpatialEffects.h"

#include <optional>

namespace rack
{
struct ReverseControls
{
    float forward, highpass, lowpass, pitch, mix;
    int note, timing;
    bool pingPong;
};

class ReverseDelay
{
public:
    void prepare(double rate, ReverseControls controls)
    {
        sampleRate = rate;
        tempo = 120;
        reportedTempo.store(tempo, std::memory_order_relaxed);
        source.prepare(rate, 2 * maxSegmentSeconds + .02);
        repeat.prepare(rate, 2 * maxSegmentSeconds + .02);
        filters = {};
        segment = position = available = previousAvailable = history = 0;
        expectedPpq.reset();
        lastBeats = 0;
        high = low = -1;
        initialise(forward, rate, controls.forward);
        initialise(pitchRatio, rate, std::exp2(controls.pitch / 12.0f));
        initialise(mix, rate, controls.mix);
        initialise(ping, rate, static_cast<float>(controls.pingPong));
        beginBlock(controls, nullptr, 0);
    }

    void beginBlock(ReverseControls controls, const juce::AudioPlayHead* playHead, int blockSize)
    {
        const auto beats = beatsForDivision(controls.note, controls.timing);
        std::optional<double> ppq;
        if (playHead != nullptr)
        {
            if (const auto host = playHead->getPosition())
            {
                if (const auto bpm = host->getBpm(); bpm && std::isfinite(*bpm) && *bpm > 0)
                {
                    tempo = *bpm;
                    reportedTempo.store(tempo, std::memory_order_relaxed);
                }
                if (const auto p = host->getPpqPosition(); host->getIsPlaying() && p && std::isfinite(*p))
                    ppq = *p;
            }
        }

        const auto seconds = std::clamp(60.0 / tempo * beats, .006, maxSegmentSeconds);
        const auto segmentSamples = static_cast<int>(std::lround(sampleRate * seconds));
        const auto discontinuity = ppq && (!expectedPpq || std::abs(*ppq - *expectedPpq) > tempo / (60 * sampleRate) * 5 + 1e-6);
        if (segmentSamples != segment || lastBeats != beats || discontinuity)
        {
            segment = segmentSamples;
            lastBeats = beats;
            position = 0;
            if (ppq)
            {
                const auto grid = *ppq * 60.0 / (tempo * seconds);
                position = static_cast<int>(std::lround((grid - std::floor(grid)) * segment)) % segment;
            }
            available = previousAvailable = history = 0;
            filters = {};
        }
        expectedPpq = ppq ? std::optional { *ppq + blockSize * tempo / (60 * sampleRate) } : std::nullopt;

        setRoutedTargets(controls);
        ping.setTargetValue(static_cast<float>(controls.pingPong));
        if (high != controls.highpass || low != controls.lowpass)
        {
            high = controls.highpass;
            low = controls.lowpass;
            const auto hp = std::tan(twoPi * .5 * std::min(high / sampleRate, .48));
            const auto lp = std::tan(twoPi * .5 * std::min(low / sampleRate, .48));
            hpf = BiquadCoefficients::pass(hp, .7071, true);
            lpf = BiquadCoefficients::pass(lp, .7071);
        }
    }

    void setRoutedTargets(ReverseControls controls)
    {
        forward.setTargetValue(controls.forward);
        pitchRatio.setTargetValue(std::exp2(controls.pitch / 12.0f));
        mix.setTargetValue(controls.mix);
    }

    double tailSeconds(ReverseControls controls) const
    {
        const auto bpm = reportedTempo.load(std::memory_order_relaxed);
        const auto seconds = std::clamp(60.0 / bpm * beatsForDivision(controls.note, controls.timing), .006, maxSegmentSeconds);
        const auto feedback = .60 * controls.forward * controls.forward;
        const auto repeats = feedback > .001 ? 1 + std::ceil(std::log(.001) / std::log(feedback)) : 1;
        return 2 * seconds * repeats + .02;
    }

    void process(std::array<float, 2>& frame, int channels, float wet)
    {
        const auto amount = forward.getNextValue();
        const auto ratio = pitchRatio.getNextValue();
        const auto alternate = channels == 2 ? ping.getNextValue() : 0.0f;
        const auto longDelay = static_cast<float>(2 * segment);
        const auto fade = std::min(sampleRate * .004, segment * .18);
        const auto edge = static_cast<float>(std::clamp(std::min(position, segment - 1 - position) / fade, 0.0, 1.0));
        const auto window = edge * edge * (3 - 2 * edge);
        const auto stereoGain = 1.0f - .5f * alternate;
        std::array<float, 2> backward {}, result {};
        for (int i = 0; i < channels; ++i)
            backward[static_cast<size_t>(i)] = readReverse(static_cast<size_t>(i), ratio) * window;

        for (int i = 0; i < channels; ++i)
        {
            const auto channel = static_cast<size_t>(i);
            const auto other = 1U - channel;
            const auto forwardTap = history >= 2 * segment ? source.read(channel, longDelay) : 0.0f;
            const auto reversed = std::lerp(backward[channel], backward[other], alternate);
            const auto first = stereoGain * (reversed + .746f * amount * forwardTap);
            auto& f = filters[channel];
            const auto shaped = static_cast<float>(f.low.process(f.high.process(first, hpf), lpf));
            const auto feedback = history >= 2 * segment ? repeat.read(channel, longDelay) : 0.0f;
            result[channel] = .60f * shaped + .60f * amount * amount * feedback;
        }
        for (int i = 0; i < channels; ++i)
        {
            const auto channel = static_cast<size_t>(i);
            source.write(channel, frame[channel]);
            repeat.write(channel, result[channel]);
        }
        source.advance();
        repeat.advance();
        mixFrames(frame, result, channels, wet * mix.getNextValue());
        history = std::min(history + 1, 2 * segment);
        ++available;
        if (++position == segment)
        {
            position = 0;
            previousAvailable = available;
            available = 0;
        }
    }

private:
    float readReverse(size_t channel, float ratio) const
    {
        const auto read = [this, channel](float offset)
        {
            offset = juce::jlimit(0.0f, static_cast<float>(segment - 1), offset);
            return offset < static_cast<float>(previousAvailable)
                ? source.read(channel, static_cast<float>(position + 1) + offset) : 0.0f;
        };

        const auto base = static_cast<float>(position);
        if (std::abs(ratio - 1.0f) < .0001f)
            return read(base);

        const auto length = static_cast<float>(std::min(sampleRate * .300, segment * .5));
        auto phaseA = base / length;
        phaseA -= std::floor(phaseA);
        const auto phaseB = phaseA < .5f ? phaseA + .5f : phaseA - .5f;
        const auto weight = static_cast<float>(.5 - .5 * std::cos(twoPi * phaseA));
        const auto offset = length * (ratio - 1.0f);
        return std::lerp(read(base + offset * (phaseB - .5f)), read(base + offset * (phaseA - .5f)), weight);
    }

    struct Filters { Biquad high, low; };
    static constexpr double maxSegmentSeconds = 12.0;
    DelayLine source, repeat;
    std::array<Filters, 2> filters;
    BiquadCoefficients hpf, lpf;
    juce::SmoothedValue<float> forward, pitchRatio, mix, ping;
    std::optional<double> expectedPpq;
    std::atomic<double> reportedTempo { 120 };
    double sampleRate = 48000, tempo = 120, lastBeats = 0;
    int segment = 0, position = 0, available = 0, previousAvailable = 0, history = 0;
    float high = -1, low = -1;
};

struct ReverbControls
{
    float preDelay, earlyLate, size, decay, highpass, lowpass, modRate, modDepth, saturation, mix;
    int type;
};

class Reverb
{
public:
    void prepare(double rate, ReverbControls controls)
    {
        sampleRate = rate;
        pre.prepare(rate, .41);
        for (auto& delay : delays)
            delay.prepare(rate, .21);
        filters = {};
        damping = {};
        phase = 0;
        high = low = -1;
        for (size_t i = 0; i < earlyModes.size(); ++i)
            initialise(earlyModes[i], rate, i == static_cast<size_t>(controls.type) ? 1.0f : 0.0f, .025);
        initialise(predelay, rate, static_cast<float>(rate * .001) * controls.preDelay, .035);
        initialise(balance, rate, controls.earlyLate);
        initialise(lateBoost, rate, earlyLateBoost(controls.type));
        initialise(modRate, rate, controls.modRate);
        initialise(modDepth, rate, static_cast<float>(rate * .0024) * controls.modDepth);
        initialise(saturation, rate, juce::Decibels::decibelsToGain(controls.saturation));
        initialise(mix, rate, controls.mix);
        for (size_t i = 0; i < delays.size(); ++i)
        {
            const auto time = delayTime(controls, i);
            initialise(times[i], rate, static_cast<float>(time * rate), .055);
            initialise(feedback[i], rate, feedbackLevel(time, controls.decay), .055);
        }
        setTargets(controls);
    }

    void setTargets(ReverbControls controls)
    {
        for (size_t i = 0; i < earlyModes.size(); ++i)
            earlyModes[i].setTargetValue(i == static_cast<size_t>(controls.type) ? 1.0f : 0.0f);
        predelay.setTargetValue(static_cast<float>(sampleRate * .001) * controls.preDelay);
        balance.setTargetValue(controls.earlyLate);
        lateBoost.setTargetValue(earlyLateBoost(controls.type));
        modRate.setTargetValue(controls.modRate);
        modDepth.setTargetValue(static_cast<float>(sampleRate * .0024) * controls.modDepth);
        saturation.setTargetValue(juce::Decibels::decibelsToGain(controls.saturation));
        mix.setTargetValue(controls.mix);
        if (controls.highpass != high || controls.lowpass != low)
        {
            high = controls.highpass;
            low = controls.lowpass;
            hpf = BiquadCoefficients::pass(std::tan(twoPi * .5 * std::min(high / sampleRate, .48)), .7071, true);
            lpf = BiquadCoefficients::pass(std::tan(twoPi * .5 * std::min(low / sampleRate, .48)), .7071);
            dampingCoefficient = static_cast<float>(1.0 - std::exp(-twoPi * std::min<double>(low, 6500) / sampleRate));
        }
        for (size_t i = 0; i < delays.size(); ++i)
        {
            const auto time = delayTime(controls, i);
            times[i].setTargetValue(static_cast<float>(time * sampleRate));
            feedback[i].setTargetValue(feedbackLevel(time, controls.decay));
        }
    }

    void process(std::array<float, 2>& frame, int channels, float wet, float motionSeconds = 0)
    {
        const auto offset = predelay.getNextValue();
        const auto lr = balance.getNextValue();
        const auto earlyLevel = std::min(1.0f, 2 * (1 - lr));
        const auto lateLevel = std::min(1.0f, 2 * lr) * (1.0f + earlyLevel * lateBoost.getNextValue());
        const auto depth = modDepth.getNextValue();
        const auto drive = saturation.getNextValue();
        const auto wetMix = wet * mix.getNextValue();
        const std::array input { frame[0], channels == 2 ? frame[1] : frame[0] };
        const std::array feed { readInput(input, 0, offset), readInput(input, 1, offset) };
        const auto early = reflections(input, offset, earlyLevel);

        std::array<float, 8> loop {};
        std::array<float, 2> tail {};
        constexpr std::array signs { 1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f };
        for (size_t i = 0; i < delays.size(); ++i)
        {
            const auto wobble = depth > 0 ? depth * static_cast<float>(std::sin(twoPi * (phase + static_cast<double>(i) / 8))) : 0;
            const auto movement = static_cast<float>(sampleRate) * motionSeconds * signs[i];
            const auto sample = delays[i].read(0, times[i].getNextValue() + wobble + movement);
            tail[0] += sample;
            tail[1] += sample * signs[i];
            damping[i] += dampingCoefficient * (sample - damping[i]);
            loop[i] = std::lerp(sample, damping[i], .12f) * feedback[i].getNextValue();
        }
        for (size_t span = 1; span < loop.size(); span *= 2)
        {
            for (size_t start = 0; start < loop.size(); start += 2 * span)
            {
                for (size_t i = start; i < start + span; ++i)
                {
                    const auto a = loop[i];
                    const auto b = loop[i + span];
                    loop[i] = a + b;
                    loop[i + span] = a - b;
                }
            }
        }
        for (size_t i = 0; i < delays.size(); ++i)
        {
            const auto inputChannel = i & 1U;
            const auto injection = .17f * (feed[inputChannel] + .25f * signs[i] * feed[1U - inputChannel]);
            delays[i].write(0, injection + loop[i] * .35355339f);
            delays[i].advance();
        }

        phase += modRate.getNextValue() / sampleRate;
        phase -= std::floor(phase);
        std::array<float, 2> effect {};
        for (size_t i = 0; i < effect.size(); ++i)
        {
            pre.write(i, input[i]);
            auto& f = filters[i];
            const auto combined = early[i] + lateLevel * .23f * tail[i];
            const auto filtered = static_cast<float>(f.low.process(f.high.process(combined, hpf), lpf));
            effect[i] = drive > 1 ? softDrive(filtered, drive) : filtered;
        }
        pre.advance();
        mixFrames(frame, effect, channels, wetMix);
    }

private:
    struct Reflection { double seconds; float direct, cross; };
    static constexpr std::array<std::array<Reflection, 8>, 3> taps {{
        {{ { 0, .42f, 0 }, { .00954, -.21f, .26f }, { .0179, .19f, -.12f }, { .031, -.16f, .11f },
           { .048, .13f, -.10f }, { .065, -.09f, .07f }, { .084, .06f, .05f }, { .112, -.035f, .035f } }},
        {{ { .01908, .26f, .26f }, { .0323, -.15f, -.14f }, { .054, .19f, .16f }, { .076, -.12f, .11f },
           { .096, .10f, -.093f }, { .126, .07f, .064f }, { .159, -.045f, .043f }, { .194, .026f, .025f } }},
        {{ { .03817, .017f, .018f }, { .059, -.018f, .015f }, { .074, .012f, -.013f }, { .092, -.013f, .016f },
           { .112, .024f, -.02f }, { .139, -.019f, -.017f }, { .172, .014f, .012f }, { .198, .010f, -.009f } }}
    }};
    static constexpr std::array<std::array<double, 8>, 3> lateTimes {{
        { .0239, .0301, .0367, .0409, .0469, .0539, .0607, .0691 },
        { .0498, .0571, .0617, .0713, .0797, .0861, .0973, .1087 },
        { .0861, .0917, .0997, .1103, .1223, .1349, .1499, .1643 }
    }};
    static double delayTime(ReverbControls controls, size_t i)
    {
        return lateTimes[static_cast<size_t>(controls.type)][i] * (.80 + .40 * controls.size);
    }

    static float feedbackLevel(double seconds, float decay)
    {
        return static_cast<float>(std::pow(10.0, -3.0 * seconds / (.98 * decay)));
    }

    static float earlyLateBoost(int type)
    {
        constexpr std::array boost { .20f, .65f, 0.0f };
        return boost[static_cast<size_t>(type)];
    }

    float readInput(const std::array<float, 2>& input, size_t channel, float samples) const
    {
        return samples < 1.0f ? std::lerp(input[channel], pre.read(channel, 1), samples) : pre.read(channel, samples);
    }

    std::array<float, 2> reflections(const std::array<float, 2>& input, float offset, float level)
    {
        std::array<float, 2> result {};
        for (size_t mode = 0; mode < earlyModes.size(); ++mode)
        {
            const auto gain = level * earlyModes[mode].getNextValue();
            if (gain == 0)
                continue;
            for (const auto& tap : taps[mode])
            {
                const auto distance = offset + static_cast<float>(sampleRate * tap.seconds);
                const auto left = readInput(input, 0, distance);
                const auto right = readInput(input, 1, distance);
                result[0] += gain * (tap.direct * left + tap.cross * right);
                result[1] += gain * (tap.direct * right + tap.cross * left);
            }
        }
        return result;
    }

    struct Filters { Biquad high, low; };
    DelayLine pre;
    std::array<DelayLine, 8> delays;
    std::array<juce::SmoothedValue<float>, 8> times, feedback;
    std::array<juce::SmoothedValue<float>, 3> earlyModes;
    std::array<Filters, 2> filters;
    std::array<float, 8> damping {};
    BiquadCoefficients hpf, lpf;
    juce::SmoothedValue<float> predelay, balance, lateBoost, modRate, modDepth, saturation, mix;
    double sampleRate = 48000, phase = 0;
    float high = -1, low = -1, dampingCoefficient = 0;
};
}
