#pragma once

#include "Effects.h"

#include <vector>

namespace rack
{
constexpr auto twoPi = juce::MathConstants<double>::twoPi;

struct BiquadCoefficients
{
    static BiquadCoefficients pass(double k, double q, bool highPass = false)
    {
        const auto kSquared = k * k;
        const auto norm = 1.0 / (1.0 + k / q + kSquared);
        const auto b0 = (highPass ? 1.0 : kSquared) * norm;
        return { b0, (highPass ? -2.0 : 2.0) * b0, b0,
                 2.0 * (kSquared - 1.0) * norm, (1.0 - k / q + kSquared) * norm };
    }

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

struct Biquad
{
    double process(double input, const BiquadCoefficients& c)
    {
        const auto output = input * c.b0 + first;
        first = input * c.b1 + second - output * c.a1;
        second = input * c.b2 - output * c.a2;
        return output;
    }

    double first = 0, second = 0;
};

inline float softDrive(float sample, float gain)
{
    return std::tanh(sample * gain) / std::sqrt(gain);
}

inline void mixFrames(std::array<float, 2>& frame, const std::array<float, 2>& effect, int channels, float mix)
{
    for (int i = 0; i < channels; ++i)
    {
        const auto channel = static_cast<size_t>(i);
        frame[channel] = std::lerp(frame[channel], effect[channel], mix);
    }
}

struct ResamplerControls
{
    float rate, drive, jitter;
    bool preFilter, postFilter, rateRouted;
};

class Resampler
{
public:
    void prepare(double sampleRate, ResamplerControls controls)
    {
        rate = sampleRate;
        channels = {};
        clock = 0.0;
        nextTick = 1.0;
        random.setSeed(juce::Random::getSystemRandom().nextInt64());
        initial = true;
        filterRate = legacyRate = -1;
        routed = controls.rateRouted;
        initialise(drive, rate, driveGain(controls.drive));
        initialise(preWet, rate, static_cast<float>(controls.preFilter));
        initialise(postWet, rate, static_cast<float>(controls.postFilter));
        initialise(jitter, rate, controls.jitter);
        initialise(movingFilter, rate, static_cast<float>(routed), .006);
        setTargets(controls);
    }

    void setTargets(ResamplerControls controls)
    {
        const auto target = std::min(rate, static_cast<double>(controls.rate));
        tickStep = target / rate;
        drive.setTargetValue(driveGain(controls.drive));
        jitter.setTargetValue(controls.jitter);
        preWet.setTargetValue(static_cast<float>(controls.preFilter));
        postWet.setTargetValue(static_cast<float>(controls.postFilter));
        if (routed != controls.rateRouted)
        {
            for (auto& channel : channels)
            {
                if (controls.rateRouted)
                    channel.preMoving = channel.postMoving = {};
                else
                    channel.pre = channel.post = {};
            }
            routed = controls.rateRouted;
        }
        movingFilter.setTargetValue(static_cast<float>(routed));
        const auto updateLegacy = (!routed || legacyRate < 0) && target != legacyRate;
        const auto updateMoving = (routed || movingFilter.getCurrentValue() != 0) && target != filterRate;
        if (!(updateLegacy || updateMoving))
            return;

        const auto warped = std::tan(twoPi * .5 * std::min(target * .5 / rate, .48));
        // Each pair is the measured, fitted pole frequency ratio and Q.
        constexpr std::array ratios { .3000, .3424, .5374, .7168, .8948, 1.1107 };
        constexpr std::array qs { .4713, 1.0595, 2.0954, 2.9386, 3.5302, 3.7063 };
        for (size_t i = 0; i < ratios.size(); ++i)
        {
            const auto pole = ratios[i] * warped;
            if (updateLegacy)
                filter[i] = BiquadCoefficients::pass(pole, qs[i]);
            if (updateMoving)
                variable[i] = IntegratorCoefficients::make(pole, qs[i]);
        }
        if (updateLegacy)
            legacyRate = target;
        if (updateMoving)
            filterRate = target;
    }

    void process(std::array<float, 2>& frame, int numChannels, float wet, float motion = 0)
    {
        const auto amount = jitter.getNextValue();
        const auto multiplier = drive.getNextValue();
        const auto pre = preWet.getNextValue();
        const auto post = postWet.getNextValue();
        const auto moving = movingFilter.getNextValue();
        const auto update = initial || clock >= nextTick;
        if (update)
        {
            if (!initial)
                clock -= nextTick;
            initial = false;
            nextTick = amount > 0 ? 1.0 + amount * .3 * (2 * random.nextFloat() - 1) : 1.0;
        }
        clock += std::min(1.0, tickStep * (1.0 + motion));
        for (int i = 0; i < numChannels; ++i)
        {
            const auto index = static_cast<size_t>(i);
            auto& channel = channels[index];
            auto& sample = frame[index];
            const auto driven = multiplier > 1 ? softDrive(sample, multiplier) : sample;
            const auto bandLimited = processFilter(channel.pre, channel.preMoving, driven, moving);
            if (update)
                channel.held = std::lerp(driven, bandLimited, pre);
            const auto reconstructed = processFilter(channel.post, channel.postMoving, channel.held, moving);
            sample = std::lerp(sample, std::lerp(channel.held, reconstructed, post), wet);
        }
    }

private:
    using FilterState = std::array<Biquad, 6>;
    struct IntegratorCoefficients
    {
        static IntegratorCoefficients make(double frequency, double q)
        {
            const auto a = 1.0 / (1.0 + frequency * (frequency + 1.0 / q));
            return { a, frequency * a, frequency * frequency * a };
        }
        double a = 1, b = 0, c = 0;
    };
    struct Integrator
    {
        double process(double sample, const IntegratorCoefficients& coefficients)
        {
            const auto difference = sample - second;
            const auto band = coefficients.a * first + coefficients.b * difference;
            const auto low = second + coefficients.b * first + coefficients.c * difference;
            first = 2 * band - first;
            second = 2 * low - second;
            return low;
        }
        double first = 0, second = 0;
    };
    using IntegratorState = std::array<Integrator, 6>;
    struct Channel { FilterState pre, post; IntegratorState preMoving, postMoving; float held = 0; };

    static float driveGain(float decibels) { return juce::Decibels::decibelsToGain(decibels); }

    float processFilter(FilterState& state, IntegratorState& movingState, float sample, float moving) const
    {
        auto current = static_cast<double>(sample);
        if (moving < 1)
            for (size_t i = 0; i < state.size(); ++i)
                current = state[i].process(current, filter[i]);
        if (moving == 0)
            return static_cast<float>(current);

        auto modulated = static_cast<double>(sample);
        for (size_t i = 0; i < movingState.size(); ++i)
            modulated = movingState[i].process(modulated, variable[i]);
        return std::lerp(static_cast<float>(current), static_cast<float>(modulated), moving);
    }

    std::array<Channel, 2> channels;
    std::array<BiquadCoefficients, 6> filter;
    std::array<IntegratorCoefficients, 6> variable;
    juce::SmoothedValue<float> drive, jitter, preWet, postWet, movingFilter;
    juce::Random random;
    double rate = 48000, clock = 0, tickStep = 1, nextTick = 1, filterRate = -1, legacyRate = -1;
    bool initial = true, routed = false;
};

class DelayLine
{
public:
    void prepare(double rate, double maxSeconds)
    {
        capacity = static_cast<int>(std::ceil(rate * maxSeconds)) + 3;
        index = 0;
        for (auto& channel : lines)
            channel.assign(static_cast<size_t>(capacity), 0.0f);
    }

    float read(size_t channel, float samples) const
    {
        const auto delay = juce::jlimit(1.0f, static_cast<float>(capacity - 2), samples);
        const auto whole = static_cast<int>(delay);
        const auto recent = index >= whole ? index - whole : index - whole + capacity;
        const auto earlier = recent > 0 ? recent - 1 : capacity - 1;
        const auto& line = lines[channel];
        return std::lerp(line[static_cast<size_t>(recent)], line[static_cast<size_t>(earlier)],
                         delay - static_cast<float>(whole));
    }

    void write(size_t channel, float sample) { lines[channel][static_cast<size_t>(index)] = sample; }
    void advance() { if (++index == capacity) index = 0; }

private:
    std::array<std::vector<float>, 2> lines;
    int capacity = 0, index = 0;
};

struct ChorusControls
{
    float depth, rate, saturation, analog, width, mix;
    bool sine, stereo;
};

class Chorus
{
public:
    void prepare(double rate, ChorusControls controls)
    {
        sampleRate = rate;
        phase = 0;
        analogPhase = 0;
        line.prepare(rate, .035);
        filtered = {};
        lowpass = static_cast<float>(1.0 - std::exp(-twoPi * 5300.0 / rate));
        initialise(depth, rate, controls.depth);
        initialise(lfoRate, rate, controls.rate);
        initialise(saturation, rate, controls.saturation);
        initialise(analog, rate, controls.analog);
        initialise(width, rate, controls.width);
        initialise(mix, rate, controls.mix);
        initialise(sine, rate, static_cast<float>(controls.sine));
        initialise(stereo, rate, static_cast<float>(controls.stereo));
    }

    void setTargets(ChorusControls controls)
    {
        depth.setTargetValue(controls.depth);
        lfoRate.setTargetValue(controls.rate);
        saturation.setTargetValue(controls.saturation);
        analog.setTargetValue(controls.analog);
        width.setTargetValue(controls.width);
        mix.setTargetValue(controls.mix);
        sine.setTargetValue(static_cast<float>(controls.sine));
        stereo.setTargetValue(static_cast<float>(controls.stereo));
    }

    void process(std::array<float, 2>& frame, int channels, float wet)
    {
        const auto amount = depth.getNextValue();
        const auto analogAmount = analog.getNextValue();
        const auto waveform = sine.getNextValue();
        const auto sideMix = width.getNextValue();
        const auto right = stereo.getNextValue();
        const auto saturationLevel = saturation.getNextValue();
        const auto gain = juce::Decibels::decibelsToGain(saturationLevel);
        std::array<float, 2> chorus {};
        const auto waveformAt = [waveform](double p)
        {
            p -= std::floor(p);
            const auto triangle = 1.0 - std::abs(2 * p - 1.0);
            const auto sinusoid = .5 - .5 * std::cos(twoPi * p);
            return static_cast<float>(std::lerp(triangle, sinusoid, waveform));
        };
        const auto leftLfo = waveformAt(phase);
        const auto rightLfo = std::lerp(leftLfo, waveformAt(phase + .5), right);
        for (int i = 0; i < channels; ++i)
        {
            const auto channel = static_cast<size_t>(i);
            const auto delay = static_cast<float>(sampleRate * (.00015 + .03 * amount * (i == 0 ? leftLfo : rightLfo)));
            const auto sample = line.read(channel, delay);
            line.write(channel, frame[channel]);
            filtered[channel] += lowpass * (sample - filtered[channel]);
            chorus[channel] = saturationLevel > 0 ? softDrive(filtered[channel], gain) : filtered[channel];
        }
        if (channels == 2)
        {
            const auto mid = .5f * (chorus[0] + chorus[1]);
            const auto side = .5f * (chorus[0] - chorus[1]) * std::lerp(.55f, 1.65f, sideMix);
            chorus = { mid + side, mid - side };
        }
        line.advance();
        mixFrames(frame, chorus, channels, wet * mix.getNextValue());
        phase += lfoRate.getNextValue() * (1 + .035 * analogAmount * std::sin(twoPi * analogPhase)) / sampleRate;
        phase -= std::floor(phase);
        analogPhase += .17 / sampleRate;
        analogPhase -= std::floor(analogPhase);
    }

private:
    DelayLine line;
    std::array<float, 2> filtered {};
    juce::SmoothedValue<float> depth, lfoRate, saturation, analog, width, mix, sine, stereo;
    double sampleRate = 48000, phase = 0, analogPhase = 0;
    float lowpass = 0;
};

struct DelayControls
{
    float warp, milliseconds, feedback, saturation, analog, highpass, lowpass, modRate, modDepth, mix;
    int note, timing;
    bool pingPong, sync, steep, freeze;
};

class Delay
{
public:
    static constexpr float maximumFeedback = .98f;

    static double repeatSeconds(DelayControls controls, double bpm)
    {
        const auto seconds = controls.sync ? 60.0 / bpm * beatsForDivision(controls.note, controls.timing)
                                           : controls.milliseconds * .001;
        return std::min(15.9, controls.warp * seconds);
    }

    double currentTempo() const { return tempo; }

    void prepare(double rate, DelayControls controls)
    {
        sampleRate = rate;
        tempo = 120;
        phase = 0;
        frozenDelay = 0;
        line.prepare(rate, maximumSeconds);
        filters = {};
        high = low = -1;
        initialise(time, rate, delaySamples(controls), .035);
        initialise(feedback, rate, controls.feedback);
        initialise(saturation, rate, juce::Decibels::decibelsToGain(controls.saturation));
        initialise(modulationRate, rate, controls.modRate);
        initialise(modulationDepth, rate, amount(controls));
        initialise(mix, rate, controls.mix);
        initialise(freeze, rate, static_cast<float>(controls.freeze));
        initialise(pingPong, rate, static_cast<float>(controls.pingPong));
        initialise(steep, rate, static_cast<float>(controls.steep));
        beginBlock(controls, nullptr);
    }

    void beginBlock(DelayControls controls, const juce::AudioPlayHead* playHead)
    {
        if (controls.sync && playHead != nullptr)
            if (const auto position = playHead->getPosition())
                if (const auto bpm = position->getBpm(); bpm && std::isfinite(*bpm) && *bpm > 0)
                    tempo = *bpm;
        time.setTargetValue(delaySamples(controls));
        setRoutedTargets(controls);
        modulationRate.setTargetValue(controls.modRate);
        modulationDepth.setTargetValue(amount(controls));
        freeze.setTargetValue(static_cast<float>(controls.freeze));
        pingPong.setTargetValue(static_cast<float>(controls.pingPong));
        steep.setTargetValue(static_cast<float>(controls.steep));
        if (controls.highpass != high || controls.lowpass != low)
        {
            high = controls.highpass;
            low = controls.lowpass;
            const auto hp = std::tan(twoPi * .5 * std::min(high / sampleRate, .48));
            const auto lp = std::tan(twoPi * .5 * std::min(low / sampleRate, .48));
            highCoefficients = BiquadCoefficients::pass(hp, .7071, true);
            lowCoefficients = BiquadCoefficients::pass(lp, .7071);
            steepCoefficients = { BiquadCoefficients::pass(lp, .541196), BiquadCoefficients::pass(lp, 1.30656) };
        }
    }

    void setRoutedTargets(DelayControls controls)
    {
        feedback.setTargetValue(controls.feedback);
        saturation.setTargetValue(juce::Decibels::decibelsToGain(controls.saturation));
        mix.setTargetValue(controls.mix);
    }

    void process(std::array<float, 2>& frame, int channels, float wet, float motionSeconds = 0)
    {
        auto delay = time.getNextValue() + static_cast<float>(std::sin(twoPi * phase)) * modulationDepth.getNextValue();
        delay += static_cast<float>(sampleRate) * motionSeconds;
        phase += modulationRate.getNextValue() / sampleRate;
        phase -= std::floor(phase);
        const auto hold = freeze.getNextValue();
        if (hold > 0)
        {
            // Fractional interpolation would attenuate the stored loop on each pass.
            if (frozenDelay == 0)
                frozenDelay = std::max(1.0f, std::round(delay));
            delay = std::lerp(delay, frozenDelay, hold);
        }
        else
        {
            frozenDelay = 0;
        }
        const auto ping = channels == 2 ? pingPong.getNextValue() : 0.0f;
        const auto slope = steep.getNextValue();
        const auto inputDrive = saturation.getNextValue();
        const auto loopGain = std::min(feedback.getNextValue(), maximumFeedback);
        std::array<float, 2> filtered {}, taps {}, audible {};
        for (int i = 0; i < channels; ++i)
        {
            const auto channel = static_cast<size_t>(i);
            auto& f = filters[channel];
            const auto tap = line.read(channel, delay);
            taps[channel] = tap;
            const auto cleaned = f.high.process(tap, highCoefficients);
            const auto shallow = f.low.process(cleaned, lowCoefficients);
            const auto steepened = f.steep[1].process(f.steep[0].process(cleaned, steepCoefficients[0]), steepCoefficients[1]);
            filtered[channel] = static_cast<float>(std::lerp(shallow, steepened, slope));
        }
        for (int i = 0; i < channels; ++i)
        {
            const auto channel = static_cast<size_t>(i);
            const auto other = 1U - channel;
            const auto effect = std::lerp(filtered[channel], filtered[other], ping);
            const auto frozen = std::lerp(taps[channel], taps[other], ping);
            audible[channel] = effect;
            // Drive only new audio so Feedback sets the same decay at every Saturation setting.
            const auto driven = inputDrive > 1 ? std::tanh(frame[channel] * inputDrive) : frame[channel];
            const auto input = driven * (1.0f - hold) * std::lerp(1.0f, .5f, ping);
            const auto recirculated = std::lerp(loopGain * effect, frozen, hold);
            line.write(channel, juce::jlimit(-2.0f, 2.0f, input + recirculated));
        }
        line.advance();
        mixFrames(frame, audible, channels, wet * mix.getNextValue());
    }

private:
    float delaySamples(DelayControls controls) const
    {
        return static_cast<float>(repeatSeconds(controls, tempo) * sampleRate);
    }

    float amount(DelayControls controls) const
    {
        return static_cast<float>(sampleRate * (.0196f * controls.modDepth * controls.modDepth
                                                + .002f * controls.analog));
    }

    struct Filters { Biquad high, low; std::array<Biquad, 2> steep; };
    static constexpr double maximumSeconds = 16;
    DelayLine line;
    std::array<Filters, 2> filters;
    BiquadCoefficients highCoefficients, lowCoefficients;
    std::array<BiquadCoefficients, 2> steepCoefficients;
    juce::SmoothedValue<float> time, feedback, saturation, modulationRate, modulationDepth, mix, freeze, pingPong, steep;
    double sampleRate = 48000, tempo = 120, phase = 0;
    float high = -1, low = -1, frozenDelay = 0;
};
}
