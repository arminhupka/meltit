#pragma once

#include "Effects.h"

namespace rack
{
struct TapeTextureControls
{
    float noiseGain, dropoutDepth, dropoutRate;
    int noiseType;
    bool noiseEnabled;
};

class TapeTexture
{
public:
    void prepare(double rate, TapeTextureControls controls)
    {
        sampleRate = rate;
        noiseLow = {};
        samplesUntilDrop = rate * .3;
        dropPosition = dropLength = 0;
        lowCoefficient = static_cast<float>(1.0 - std::exp(-juce::MathConstants<double>::twoPi * 85 / rate));
        normaliseLow = std::sqrt((2.0f - lowCoefficient) / lowCoefficient);
        noiseRandom.setSeed(juce::Random::getSystemRandom().nextInt64());
        dropoutRandom.setSeed(juce::Random::getSystemRandom().nextInt64());
        type = controls.noiseType;
        initialise(noiseLevel, rate, level(controls));
        initialise(dropout, rate, controls.dropoutDepth);
        dropoutRate = controls.dropoutRate;
    }

    void setTargets(TapeTextureControls controls)
    {
        type = controls.noiseType;
        noiseLevel.setTargetValue(level(controls));
        dropout.setTargetValue(controls.dropoutDepth);
        dropoutRate = controls.dropoutRate;
    }

    void process(std::array<float, 2>& frame, int channels)
    {
        const auto depth = dropout.getNextValue();
        const auto level = noiseLevel.getNextValue();
        if (depth == 0.0f && level == 0.0f)
            return;

        const auto envelope = depth > 0 ? dropoutEnvelope() : 0.0f;
        const auto gain = 1.0f - (0.84f * depth * envelope);
        constexpr std::array bass { .86f, .93f, .94f, .52f, .995f };
        constexpr std::array treble { .50f, .37f, .35f, .85f, .10f };
        const auto index = static_cast<size_t>(type);
        for (int i = 0; i < channels; ++i)
        {
            const auto channel = static_cast<size_t>(i);
            auto& sample = frame[channel];
            sample *= gain;
            if (level > 0)
            {
                const auto white = 1.7320508f * (2.0f * noiseRandom.nextFloat() - 1.0f);
                auto& low = noiseLow[channel];
                low += lowCoefficient * (white - low);
                const auto coloured = bass[index] * normaliseLow * low + treble[index] * (white - low);
                sample += level * coloured;
            }
        }
    }

private:
    static float level(TapeTextureControls controls)
    {
        constexpr std::array refOffsets { -40.41f, -42.26f, -31.34f, -33.07f, -46.50f };
        return controls.noiseEnabled ? juce::Decibels::decibelsToGain(
            controls.noiseGain + refOffsets[static_cast<size_t>(controls.noiseType)], -140.0f) : 0.0f;
    }

    float dropoutEnvelope()
    {
        if (dropPosition < dropLength)
        {
            const auto shape = std::sin(juce::MathConstants<double>::pi * dropPosition / dropLength);
            ++dropPosition;
            return static_cast<float>(shape * shape);
        }
        if (--samplesUntilDrop <= 0)
        {
            dropPosition = 0;
            dropLength = sampleRate * (0.95 + .85 * dropoutRandom.nextFloat()) / dropoutRate;
            samplesUntilDrop = sampleRate * (.20 + .7 * dropoutRandom.nextFloat()) / dropoutRate;
        }
        return 0.0f;
    }

    juce::Random noiseRandom, dropoutRandom;
    juce::SmoothedValue<float> noiseLevel, dropout;
    std::array<float, 2> noiseLow {};
    double sampleRate = 48000, samplesUntilDrop = 0, dropPosition = 0, dropLength = 0;
    float lowCoefficient = 0, normaliseLow = 1, dropoutRate = 2.3f;
    int type = 0;
};
}
