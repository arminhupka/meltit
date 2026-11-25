#include <juce_audio_processors/juce_audio_processors.h>

#include "Modulation.h"
#include "TimeStretch.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 240;

class PlayHead final : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override { return position; }

    void set(double bpm, double ppq)
    {
        position.setBpm(bpm);
        position.setPpqPosition(ppq);
        position.setIsPlaying(true);
    }

    void setMeter(int numerator, int denominator)
    {
        position.setTimeSignature(TimeSignature { numerator, denominator });
    }

private:
    PositionInfo position;
};

void setParameter(juce::AudioProcessor& processor, const juce::String& id, float value)
{
    for (auto* parameter : processor.getParameters())
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter); ranged != nullptr && ranged->paramID == id)
        {
            ranged->setValueNotifyingHost(ranged->convertTo0to1(value));
            return;
        }
    }
    throw std::runtime_error("Missing parameter: " + id.toStdString());
}

void verifyPhase(juce::AudioProcessor& processor, PlayHead& playHead, double bpm, double firstPpq, double noteLength)
{
    juce::AudioBuffer<float> audio(2, blockSize);
    juce::MidiBuffer midi;
    for (int block = 0; block < 200; ++block)
    {
        const auto ppq = firstPpq + block * blockSize * bpm / (60.0 * sampleRate);
        playHead.set(bpm, ppq);
        for (int channel = 0; channel < 2; ++channel)
            std::fill_n(audio.getWritePointer(channel), blockSize, 0.5f);
        processor.processBlock(audio, midi);
        const auto cycles = (ppq + (blockSize - 1) * bpm / (60.0 * sampleRate)) / noteLength;
        const auto phase = cycles - std::floor(cycles);
        const auto expected = phase < 0.5 ? phase : 1.0 - phase;
        if (std::abs(audio.getSample(0, blockSize - 1) - expected) > 0.018)
            throw std::runtime_error("Tremolo phase did not follow BPM and PPQ from the host");
    }
}

void verifyDelayTiming(juce::AudioProcessor& processor, PlayHead& playHead, double bpm, int timing, double noteLength)
{
    processor.releaseResources();
    setParameter(processor, "tremolo_active", 0.0f);
    setParameter(processor, "delay_active", 1.0f);
    setParameter(processor, "delay_note", 3.0f);
    setParameter(processor, "delay_timing", static_cast<float>(timing));
    setParameter(processor, "delay_mix", 100.0f);
    setParameter(processor, "delay_feedback", 0.0f);
    setParameter(processor, "delay_saturation", 0.0f);
    processor.prepareToPlay(sampleRate, blockSize);

    constexpr int pulse = 24000;
    juce::AudioBuffer<float> audio(2, blockSize);
    juce::MidiBuffer midi;
    float peak = 0;
    int actual = 0;
    for (int start = 0; start < 60000; start += blockSize)
    {
        playHead.set(bpm, start * bpm / (60.0 * sampleRate));
        audio.clear();
        if (start == pulse)
            audio.setSample(0, 0, .25f);
        processor.processBlock(audio, midi);
        for (int i = 0; i < blockSize; ++i)
        {
            const auto sample = std::abs(audio.getSample(0, i));
            if (sample > peak)
            {
                peak = sample;
                actual = start + i;
            }
            if (std::abs(audio.getSample(1, i)) > 1.0e-7f)
                throw std::runtime_error("Standard Delay must preserve the echo channel");
        }
    }
    const auto expected = pulse + std::lround(sampleRate * 60.0 / bpm * noteLength);
    if (peak < .02f || std::abs(actual - expected) > 10)
        throw std::runtime_error("Delay repeat did not follow tempo and note timing");
}

void verifyReverseTiming(juce::AudioProcessor& processor, PlayHead& playHead, bool seek)
{
    processor.releaseResources();
    setParameter(processor, "delay_active", 0.0f);
    setParameter(processor, "reverse_active", 1.0f);
    setParameter(processor, "reverse_note", 3.0f);
    setParameter(processor, "reverse_mix", 100.0f);
    setParameter(processor, "reverse_forward", 0.0f);
    processor.prepareToPlay(sampleRate, blockSize);
    juce::AudioBuffer<float> audio(2, blockSize);
    juce::MidiBuffer midi;
    float peak = 0;
    int actual = 0;
    for (int start = 0; start < 30000; start += blockSize)
    {
        const auto ppq = .125 + start * 2 / sampleRate + (seek && start >= 5760 ? 8 : 0);
        playHead.set(120.0, ppq);
        audio.clear();
        if (start == 4800)
            audio.setSample(0, 0, .25f);
        processor.processBlock(audio, midi);
        for (int i = 0; i < blockSize; ++i)
        {
            const auto sample = std::abs(audio.getSample(0, i));
            if (sample > peak)
            {
                peak = sample;
                actual = start + i;
            }
        }
    }
    if (seek ? peak > 1e-6f : (peak < .03f || std::abs(actual - 13200) > 10))
        throw std::runtime_error("Reverse Delay did not follow beat position or transport seek");
}

// A 440 Hz tone steps up to 880 Hz after 0.25 s. At 4x the stretched output must keep playing the 440 Hz
// material, at its original pitch, until the LIVE point at half a bar, then rejoin the input exactly.
void verifyStretch(PlayHead& playHead, int engine)
{
    rack::StretchControls controls;
    controls.ratio = 4;
    controls.start = 0;
    controls.land = .5f;
    controls.length = 2;
    controls.engine = engine;
    rack::TimeStretch stretch;
    stretch.prepare(sampleRate, controls);
    playHead.setMeter(4, 4);

    constexpr int total = 96000;
    std::vector<float> input(total), output(total);
    double phase = 0;
    for (int i = 0; i < total; ++i)
    {
        phase += (i < 12000 ? 440.0 : 880.0) / sampleRate;
        input[static_cast<size_t>(i)] = .5f * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * phase));
    }
    for (int start = 0; start < total; start += blockSize)
    {
        playHead.set(120.0, start * 2 / sampleRate);
        stretch.beginBlock(controls, &playHead, blockSize);
        for (int i = start; i < start + blockSize; ++i)
        {
            std::array frame { input[static_cast<size_t>(i)], input[static_cast<size_t>(i)] };
            stretch.process(frame, 2, 1.0f);
            if (!std::isfinite(frame[0]) || std::abs(frame[0] - frame[1]) > 1.0e-6f)
                throw std::runtime_error("Stretch produced a non-finite or unbalanced sample");
            output[static_cast<size_t>(i)] = frame[0];
        }
    }

    const auto frequency = [&output](int from, int to)
    {
        int crossings = 0;
        double energy = 0;
        for (int i = from + 1; i < to; ++i)
        {
            crossings += output[static_cast<size_t>(i - 1)] < 0 && output[static_cast<size_t>(i)] >= 0;
            energy += output[static_cast<size_t>(i)] * output[static_cast<size_t>(i)];
        }
        return std::pair { crossings * sampleRate / (to - from), std::sqrt(energy / (to - from)) };
    };
    const auto power = [&output](double hz, int from, int to)
    {
        const auto coefficient = 2 * std::cos(juce::MathConstants<double>::twoPi * hz / sampleRate);
        double previous = 0, older = 0;
        for (int i = from; i < to; ++i)
        {
            const auto next = output[static_cast<size_t>(i)] + coefficient * previous - older;
            older = previous;
            previous = next;
        }
        return previous * previous + older * older - coefficient * previous * older;
    };
    // Aligned engines join cycles in phase, so even zero crossings stay at 440 Hz. Every engine must put the
    // held material at 440 Hz rather than at the current input (880 Hz) or a tape-style slowdown (110 Hz).
    const auto [held, heldLevel] = frequency(28800, 45600);
    const auto aligned = engine == rack::TimeStretch::smooth || engine == rack::TimeStretch::nuskool;
    const auto original = power(440, 28800, 45600);
    if ((aligned && std::abs(held - 440.0) > 440.0 * .03) || original < 10 * power(880, 28800, 45600)
        || original < 10 * power(110, 28800, 45600) || heldLevel < .5 * .3536 || heldLevel > 1.5 * .3536)
        throw std::runtime_error("Stretch did not hold the earlier material at its original pitch (engine "
                                 + std::to_string(engine) + ", " + std::to_string(held) + " Hz)");
    for (int i = 49200; i < 95000; ++i)
        if (std::abs(output[static_cast<size_t>(i)] - input[static_cast<size_t>(i)]) > 1.0e-5f)
            throw std::runtime_error("Stretch did not land back on LIVE audio (engine " + std::to_string(engine) + ")");
}

void verifyLfoPosition(PlayHead& playHead)
{
    rack::ModulationSettings settings;
    settings.lfos[0].division = 6;
    settings.lfos[0].routes[0] = { rack::saturationMixTarget, .5f };
    settings.lfos[1].division = 4;
    settings.lfos[1].routes[0] = { rack::reverseForwardTarget, -.5f };
    rack::Modulation modulation;
    modulation.prepare(sampleRate, settings);
    playHead.setMeter(3, 4);
    playHead.set(96, .75);
    modulation.beginBlock(settings, &playHead);
    modulation.update(0);
    if (std::abs(modulation.offsets()[rack::saturationMixTarget] - .5f) > .0001f ||
        std::abs(modulation.offsets()[rack::reverseForwardTarget] - .5f) > .0001f)
        throw std::runtime_error("LFOs did not align to their own host divisions and meter");
    modulation.endBlock(32);
    playHead.set(96, 2.25);
    modulation.beginBlock(settings, &playHead);
    for (int i = 0; i < 32; ++i)
        modulation.update(i);
    if (std::abs(modulation.offsets()[rack::saturationMixTarget] + .5f) > .002f ||
        std::abs(modulation.offsets()[rack::reverseForwardTarget] + .5f) > .002f)
        throw std::runtime_error("LFO phase did not resync after a transport jump");
}
}

int main()
{
    try
    {
        juce::ScopedJuceInitialiser_GUI runtime;
        std::unique_ptr<juce::AudioProcessor> processor(createPluginFilter());
        PlayHead playHead;
        processor->setPlayHead(&playHead);
        setParameter(*processor, "tape_active", 0.0f);
        setParameter(*processor, "tremolo_active", 1.0f);
        setParameter(*processor, "tremolo_depth", 100.0f);
        setParameter(*processor, "tremolo_note", 4.0f);
        processor->prepareToPlay(sampleRate, blockSize);
        verifyPhase(*processor, playHead, 90.0, 0.25, 1.0);
        setParameter(*processor, "tremolo_timing", 1.0f);
        verifyPhase(*processor, playHead, 137.0, 4.25, 2.0 / 3.0);
        verifyDelayTiming(*processor, playHead, 96.0, 0, .50);
        verifyDelayTiming(*processor, playHead, 135.0, 2, .75);
        verifyReverseTiming(*processor, playHead, false);
        verifyReverseTiming(*processor, playHead, true);
        verifyLfoPosition(playHead);
        for (int engine = 0; engine < 4; ++engine)
            verifyStretch(playHead, engine);
        processor->setPlayHead(nullptr);
        std::cout << "Host BPM, PPQ, LFO meter/seek, Tremolo, Delay, Reverse timing and Stretch passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
