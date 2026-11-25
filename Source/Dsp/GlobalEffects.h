#pragma once

#include "SpatialEffects.h"

#include <signalsmith-stretch.h>

namespace rack
{
struct GlobalControls
{
    float inputDb, outputDb, width, pitch;
    bool eqEnabled;
    std::array<float, 3> frequency, gainDb;
};

class GlobalPitch
{
public:
    void prepare(double rate, int channels, float pitch)
    {
        const auto window = static_cast<int>(std::lround(rate * .088));
        const auto hop = static_cast<int>(std::lround(rate * .011));
        stretch.configure(channels, window, hop, false);
        latency = stretch.inputLatency() + stretch.outputLatency();
        tail.store(static_cast<double>(latency) / rate, std::memory_order_relaxed);
        warmSamples = 0;
        running = pitch != 0;
        initialise(shift, rate, pitch, .025);
        initialise(wet, rate, running ? 1.0f : 0.0f, .025);
        lastShift = 0;
    }

    void setPitch(float pitch)
    {
        if (pitch == 0)
        {
            warmSamples = 0;
            wet.setTargetValue(0.0f);
            return;
        }
        if (!running)
        {
            stretch.reset();
            running = true;
            warmSamples = latency;
            shift.setCurrentAndTargetValue(pitch);
        }
        shift.setTargetValue(pitch);
        if (warmSamples == 0)
            wet.setTargetValue(1.0f);
    }

    void process(juce::AudioBuffer<float>& audio, int channels, int count)
    {
        if (!running)
            return;

        for (int start = 0; start < count; start += chunkSize)
        {
            const auto samples = juce::jmin(chunkSize, count - start);
            const auto pitch = shift.skip(samples);
            if (pitch != lastShift)
            {
                stretch.setTransposeSemitones(pitch);
                lastShift = pitch;
            }

            std::array<const float*, 2> input {};
            std::array<float*, 2> output { scratch[0].data(), scratch[1].data() };
            for (int c = 0; c < channels; ++c)
                input[static_cast<size_t>(c)] = audio.getReadPointer(c, start);
            stretch.process(input, samples, output, samples);

            for (int i = 0; i < samples; ++i)
            {
                const auto blend = wet.getNextValue();
                if (warmSamples > 0 && --warmSamples == 0)
                    wet.setTargetValue(1.0f);
                for (int c = 0; c < channels; ++c)
                {
                    auto& original = audio.getWritePointer(c, start)[i];
                    original = std::lerp(original, scratch[static_cast<size_t>(c)][static_cast<size_t>(i)], blend);
                }
            }
            if (warmSamples == 0 && wet.getCurrentValue() == 0 && wet.getTargetValue() == 0)
            {
                running = false;
                break;
            }
        }
    }

    double tailSeconds() const { return tail.load(std::memory_order_relaxed); }

private:
    static constexpr int chunkSize = 128;
    std::array<std::array<float, chunkSize>, 2> scratch {};
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    juce::SmoothedValue<float> shift, wet;
    std::atomic<double> tail { .088 };
    float lastShift = 0;
    int latency = 0, warmSamples = 0;
    bool running = false;
};

class GlobalEffects
{
public:
    void prepare(double rate, int channels, GlobalControls settings)
    {
        sampleRate = rate;
        pitch.prepare(rate, channels, settings.pitch);
        initialise(inputGain, rate, linearGain(settings.inputDb), .015);
        initialise(outputGain, rate, linearGain(settings.outputDb), .015);
        initialise(width, rate, settings.width, .015);
        initialise(eqWet, rate, static_cast<float>(settings.eqEnabled), .015);
        for (size_t i = 0; i < bands.size(); ++i)
        {
            auto& band = bands[i];
            band.filters = {};
            band.lastFrequency = band.lastGain = 0;
            initialise(band.frequency, rate, settings.frequency[i], .03);
            initialise(band.gain, rate, settings.gainDb[i], .03);
        }
        controlInterval = 0;
    }

    void setTargets(GlobalControls settings)
    {
        pitch.setPitch(settings.pitch);
        inputGain.setTargetValue(linearGain(settings.inputDb));
        outputGain.setTargetValue(linearGain(settings.outputDb));
        width.setTargetValue(settings.width);
        eqWet.setTargetValue(static_cast<float>(settings.eqEnabled));
        for (size_t i = 0; i < bands.size(); ++i)
        {
            bands[i].frequency.setTargetValue(settings.frequency[i]);
            bands[i].gain.setTargetValue(settings.gainDb[i]);
        }
    }

    void applyInput(std::array<float, 2>& frame, int channels)
    {
        const auto gain = inputGain.getNextValue();
        for (int c = 0; c < channels; ++c)
            frame[static_cast<size_t>(c)] *= gain;
    }

    void applyOutput(juce::AudioBuffer<float>& buffer, int channels, int count)
    {
        pitch.process(buffer, channels, count);
        auto* const* audio = buffer.getArrayOfWritePointers();
        for (int i = 0; i < count; ++i)
        {
            const std::array beforeEq { audio[0][i], channels == 2 ? audio[1][i] : 0.0f };
            auto frame = beforeEq;
            const auto widthValue = width.getNextValue();
            const auto forceUpdate = controlInterval == 0;
            controlInterval = (controlInterval + 1) & 31;
            for (size_t j = 0; j < bands.size(); ++j)
            {
                auto& band = bands[j];
                const auto frequency = band.frequency.getNextValue();
                const auto gain = band.gain.getNextValue();
                if (gain == 0)
                {
                    if (band.lastGain != 0)
                        band.filters = {};
                    band.lastGain = 0;
                    continue;
                }
                if ((forceUpdate || band.lastGain == 0) && (frequency != band.lastFrequency || gain != band.lastGain))
                {
                    band.coefficients = eqCoefficients(j, frequency, gain);
                    band.lastFrequency = frequency;
                    band.lastGain = gain;
                }
                for (int c = 0; c < channels; ++c)
                {
                    const auto channel = static_cast<size_t>(c);
                    frame[channel] = static_cast<float>(band.filters[channel].process(frame[channel], band.coefficients));
                }
            }
            mixFrames(frame, beforeEq, channels, 1.0f - eqWet.getNextValue());

            if (channels == 2 && widthValue != 1)
            {
                const auto mid = .5f * (frame[0] + frame[1]);
                const auto side = .5f * (frame[0] - frame[1]) * widthValue;
                frame = { mid + side, mid - side };
            }
            const auto gain = outputGain.getNextValue();
            for (int c = 0; c < channels; ++c)
                audio[c][i] = frame[static_cast<size_t>(c)] * gain;
        }
    }

    double pitchTailSeconds() const { return pitch.tailSeconds(); }

private:
    struct Band
    {
        juce::SmoothedValue<float> frequency, gain;
        BiquadCoefficients coefficients;
        std::array<Biquad, 2> filters;
        float lastFrequency = 0, lastGain = 0;
    };

    static float linearGain(float db) { return juce::Decibels::decibelsToGain(db); }

    BiquadCoefficients eqCoefficients(size_t band, float frequency, float gain) const
    {
        // Shelf frequency and Q use the fit measured against the installed reference EQ.
        constexpr std::array scale { 1.79, 1.0, .55 };
        constexpr std::array resonance { 1.05, .70710678, 1.125 };
        const auto omega = twoPi * std::min(.47, frequency * scale[band] / sampleRate);
        const auto cosine = std::cos(omega);
        const auto amplitude = std::pow(10.0, gain / 40.0);
        const auto alpha = std::sin(omega) / (2 * resonance[band]);
        if (band == 1)
        {
            const auto norm = 1.0 / (1.0 + alpha / amplitude);
            return { (1 + alpha * amplitude) * norm, -2 * cosine * norm, (1 - alpha * amplitude) * norm,
                     -2 * cosine * norm, (1 - alpha / amplitude) * norm };
        }

        const auto beta = 2 * std::sqrt(amplitude) * alpha;
        const auto sum = amplitude + 1;
        const auto difference = amplitude - 1;
        const auto sign = band == 0 ? 1.0 : -1.0;
        const auto norm = 1.0 / (sum + sign * difference * cosine + beta);
        return { amplitude * (sum - sign * difference * cosine + beta) * norm,
                 2 * sign * amplitude * (difference - sign * sum * cosine) * norm,
                 amplitude * (sum - sign * difference * cosine - beta) * norm,
                 -2 * sign * (difference + sign * sum * cosine) * norm,
                 (sum + sign * difference * cosine - beta) * norm };
    }

    GlobalPitch pitch;
    juce::SmoothedValue<float> inputGain, outputGain, width, eqWet;
    std::array<Band, 3> bands;
    double sampleRate = 48000;
    int controlInterval = 0;
};
}
