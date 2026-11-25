#pragma once

#include "SpatialEffects.h"

#include <optional>
#include <vector>

namespace rack
{
// Live, pitch-locked cyclic time stretching. A tempo-synced program decides where audio slows down: inside the
// stretch region the read head walks through the captured input at 1/ratio speed while short source cycles
// replay at their original speed, so pitch holds. Leaving the region lands back on LIVE with a 5 ms crossfade.
struct StretchControls
{
    float ratio = 2, start = 0, land = .75f, cycle = 998, overlap = .25f, mix = 1;
    int length = 2, shape = 0, engine = 0;
};

class TimeStretch
{
public:
    enum Shape { hold, rampUp, rampDown, swell, steps };
    enum Engine { smooth, nuskool, og, hard };

    static constexpr double historySeconds = 20.0;
    static constexpr float maximumCycle = 4000;

    void prepare(double rate, StretchControls controls)
    {
        sampleRate = rate;
        maxCycleSamples = static_cast<int>(std::ceil(maximumCycle * rate / 44100.0));
        const auto reserve = 4 * maxCycleSamples + static_cast<int>(rate * .05);
        size_t size = 1;
        while (size < static_cast<size_t>(rate * historySeconds) + static_cast<size_t>(reserve))
            size <<= 1;
        mask = static_cast<int64_t>(size) - 1;
        capacity = static_cast<double>(size) - reserve;
        for (auto& channel : history)
            channel.assign(size, 0.0f);
        written = 0;
        tempo = 120;
        beatsPerBar = 4;
        phase = 0;
        expectedPpq.reset();
        wasPlaying = false;
        landingLength = std::max(1, static_cast<int>(std::lround(rate * .005)));
        resetToLive();
        initialise(mix, rate, controls.mix);
        beginBlock(controls, nullptr, 0);
    }

    void beginBlock(StretchControls controls, const juce::AudioPlayHead* playHead, int blockSize)
    {
        std::optional<double> ppq;
        bool playing = false;
        if (playHead != nullptr)
        {
            if (const auto host = playHead->getPosition())
            {
                if (const auto bpm = host->getBpm(); bpm && std::isfinite(*bpm) && *bpm > 0)
                    tempo = *bpm;
                if (const auto meter = host->getTimeSignature(); meter && meter->numerator > 0 && meter->denominator > 0)
                    beatsPerBar = meter->numerator * 4.0 / meter->denominator;
                playing = host->getIsPlaying();
                if (const auto p = host->getPpqPosition(); playing && p && std::isfinite(*p))
                    ppq = *p;
            }
        }

        setRoutedTargets(controls);
        targets.length = std::clamp(controls.length, 0, 4);
        targets.shape = std::clamp(controls.shape, 0, 4);
        targets.engine = std::clamp(controls.engine, 0, 3);
        const auto beats = programBeats();
        phaseStep = tempo / (60.0 * sampleRate * beats);
        // Published for the host, which may ask for the tail from any thread.
        tail.store(std::min(historySeconds, 60.0 / tempo * beats), std::memory_order_relaxed);

        // Stop, seek and loop jumps return to current audio; the program then follows the host position again.
        const auto tolerance = tempo / (60 * sampleRate) * 5 + 1e-6;
        const auto jumped = ppq && expectedPpq && std::abs(*ppq - *expectedPpq) > tolerance;
        if (jumped || (wasPlaying && !playing))
            requestLanding = true;
        if (ppq)
        {
            const auto cycles = *ppq / beats;
            phase = cycles - std::floor(cycles);
            expectedPpq = *ppq + blockSize * tempo / (60 * sampleRate);
        }
        else
            expectedPpq.reset();
        wasPlaying = playing;

        // Published once per block for the editor's map display.
        shownPhase.store(static_cast<float>(phase), std::memory_order_relaxed);
        shownDebt.store(static_cast<float>(debtSamples() / sampleRate), std::memory_order_relaxed);
        shownRatio.store(stretching ? lastRatio : 1.0f, std::memory_order_relaxed);
        shownStretching.store(stretching, std::memory_order_relaxed);
    }

    float displayPhase() const { return shownPhase.load(std::memory_order_relaxed); }
    float displayDebtSeconds() const { return shownDebt.load(std::memory_order_relaxed); }
    float displayRatio() const { return shownRatio.load(std::memory_order_relaxed); }
    bool displayStretching() const { return shownStretching.load(std::memory_order_relaxed); }

    void setRoutedTargets(StretchControls controls)
    {
        targets.ratio = std::clamp(controls.ratio, 1.0f, 20.0f);
        targets.start = std::clamp(controls.start, 0.0f, 1.0f);
        targets.land = std::clamp(controls.land, 0.0f, 1.0f);
        targets.cycle = std::clamp(controls.cycle, 20.0f, maximumCycle);
        targets.overlap = std::clamp(controls.overlap, 0.0f, .5f);
        mix.setTargetValue(std::clamp(controls.mix, 0.0f, 1.0f));
    }

    double tailSeconds() const { return tail.load(std::memory_order_relaxed); }

    bool isStretching() const { return stretching; }
    double debtSamples() const { return stretching ? static_cast<double>(written - 1) - readPosition : 0.0; }

    // Ratio the drawn program asks for at a position in [0, 1); 0 means LIVE.
    static float ratioAt(const StretchControls& c, double position)
    {
        auto span = c.land - c.start;
        span -= std::floor(span);
        if (span < 1.0e-4f)
            span = 1; // START on LAND: the whole program stretches and only protective returns land it.
        auto offset = static_cast<float>(position) - c.start;
        offset -= std::floor(offset);
        if (offset >= span)
            return 0;
        const auto x = offset / span;
        float exponent = 1;
        switch (c.shape)
        {
            case rampUp: exponent = x; break;
            case rampDown: exponent = 1 - x; break;
            case swell: exponent = std::sin(juce::MathConstants<float>::pi * x); break;
            case steps: exponent = (std::floor(x * 4) + 1) * .25f; break;
            default: break;
        }
        return std::pow(c.ratio, exponent);
    }

    void process(std::array<float, 2>& frame, int channels, float wet)
    {
        const auto now = written;
        for (int c = 0; c < 2; ++c)
            history[static_cast<size_t>(c)][static_cast<size_t>(now & mask)] = frame[static_cast<size_t>(c < channels ? c : 0)];
        ++written;

        const auto position = phase;
        phase += phaseStep;
        phase -= std::floor(phase);
        // A powered-down cartridge only keeps its history and clock running, so switching it on needs no warm-up.
        if (wet <= 0.0f)
        {
            if (stretching || landing > 0)
                resetToLive();
            requestLanding = false;
            mix.skip(1);
            return;
        }

        const auto ratio = ratioAt(targets, position);
        lastRatio = ratio > 0 ? ratio : 1.0f;

        if (stretching && (ratio <= 0 || requestLanding || static_cast<double>(now) - readPosition > capacity))
            land();
        requestLanding = false;
        if (!stretching && landing == 0 && ratio > 0)
            begin(now);

        const auto amount = wet * mix.getNextValue();
        if (!stretching && landing == 0)
            return;

        readPosition = std::min(readPosition + (stretching ? 1.0 / ratio : 1.0), static_cast<double>(now));
        auto output = renderGrains(now);
        if (landing > 0)
        {
            const auto g = static_cast<float>(landing) / static_cast<float>(landingLength);
            const auto keep = std::sin(g * juce::MathConstants<float>::halfPi);
            const auto rejoin = std::cos(g * juce::MathConstants<float>::halfPi);
            for (int c = 0; c < channels; ++c)
                output[static_cast<size_t>(c)] = keep * output[static_cast<size_t>(c)] + rejoin * frame[static_cast<size_t>(c)];
            if (--landing == 0)
                resetToLive();
        }
        mixFrames(frame, output, channels, amount);
    }

private:
    struct Grain
    {
        int64_t start = 0;
        int age = 0, length = 0, fadeIn = 0, fadeOut = 0;
        bool active = false;
    };

    struct EngineSpec
    {
        bool align, equalPower;
        double minimumFadeSeconds;
        int minimumFadeSamples;
        float search;
    };

    static EngineSpec spec(int engine)
    {
        switch (engine)
        {
            case nuskool: return { true, false, 0, 32, .25f };
            case og: return { false, true, 0, 48, 0 };
            case hard: return { false, false, 0, 4, 0 };
            default: return { true, false, .002, 0, .5f };
        }
    }

    double programBeats() const
    {
        constexpr std::array bars { .25, .5, 1.0, 2.0, 4.0 };
        return bars[static_cast<size_t>(targets.length)] * beatsPerBar;
    }

    float sample(size_t channel, int64_t index) const { return history[channel][static_cast<size_t>(index & mask)]; }
    float mono(int64_t index) const { return sample(0, index) + sample(1, index); }

    void resetToLive()
    {
        stretching = false;
        landing = 0;
        grains = {};
        newest = -1;
        readPosition = 0;
    }

    void begin(int64_t now)
    {
        stretching = true;
        readPosition = static_cast<double>(now);
        grains = {};
        // The first cycle starts exactly at the write head, so the switch from LIVE is seamless.
        spawn(now, now, 0);
    }

    void land()
    {
        stretching = false;
        landing = landingLength;
    }

    int cycleSamples() const
    {
        return std::clamp(static_cast<int>(std::lround(targets.cycle * sampleRate / 44100.0)), 16, maxCycleSamples);
    }

    void spawn(int64_t now, int64_t sourceStart, int fadeIn)
    {
        const auto engine = spec(targets.engine);
        const auto length = cycleSamples();
        const auto minimumFade = std::max(engine.minimumFadeSamples,
                                          static_cast<int>(std::lround(engine.minimumFadeSeconds * sampleRate)));
        const auto fadeOut = std::clamp(std::max(static_cast<int>(std::lround(targets.overlap * length)), minimumFade),
                                        1, length / 2);
        auto start = std::min(sourceStart, now);
        if (engine.align && newest >= 0)
            start = align(now, start, grains[static_cast<size_t>(newest)], length, engine.search, fadeOut);

        for (size_t i = 0; i < grains.size(); ++i)
        {
            if (!grains[i].active)
            {
                grains[i] = { start, 0, std::max(length, fadeIn + fadeOut + 1), fadeIn, fadeOut, true };
                newest = static_cast<int>(i);
                return;
            }
        }
    }

    // Waveform alignment: pick the start near the read head whose recent past best matches the material the
    // outgoing cycle is playing, so adjacent cycles join in phase.
    int64_t align(int64_t now, int64_t target, const Grain& outgoing, int length, float searchFraction, int fade) const
    {
        const auto continuation = outgoing.start + outgoing.age;
        const auto range = static_cast<int64_t>(std::max(8.0f, length * searchFraction));
        const auto low = target - range;
        const auto high = std::min(target + range, now);
        if (high <= low)
            return std::min(target, now);
        const auto window = std::clamp(fade, 32, 512);
        const auto pointStep = std::max(1, window / 48);
        const auto candidateStep = std::max<int64_t>(1, (high - low) / 96);
        auto best = std::min(target, now);
        auto bestScore = -2.0f;
        for (auto s = low; s <= high; s += candidateStep)
        {
            float cross = 0, energyA = 0, energyB = 0;
            for (int k = 1; k <= window; k += pointStep)
            {
                const auto a = mono(continuation - k);
                const auto b = mono(s - k);
                cross += a * b;
                energyA += a * a;
                energyB += b * b;
            }
            const auto score = cross / std::sqrt(energyA * energyB + 1.0e-12f);
            if (score > bestScore)
            {
                bestScore = score;
                best = s;
            }
        }
        return best;
    }

    float windowAt(const Grain& grain, bool equalPower) const
    {
        float gain = 1;
        if (grain.age < grain.fadeIn)
            gain = static_cast<float>(grain.age) / static_cast<float>(grain.fadeIn);
        const auto remaining = grain.length - grain.age;
        if (remaining <= grain.fadeOut)
            gain = std::min(gain, static_cast<float>(remaining) / static_cast<float>(grain.fadeOut));
        return equalPower ? std::sin(gain * juce::MathConstants<float>::halfPi) : gain;
    }

    std::array<float, 2> renderGrains(int64_t now)
    {
        if (newest < 0 || !grains[static_cast<size_t>(newest)].active)
            spawn(now, static_cast<int64_t>(readPosition), 0);
        else if (const auto& last = grains[static_cast<size_t>(newest)]; last.age >= last.length - last.fadeOut)
            spawn(now, static_cast<int64_t>(readPosition), last.fadeOut);

        const auto equalPower = spec(targets.engine).equalPower;
        std::array<float, 2> sum {};
        for (auto& grain : grains)
        {
            if (!grain.active)
                continue;
            const auto gain = windowAt(grain, equalPower);
            const auto index = grain.start + grain.age;
            sum[0] += gain * sample(0, index);
            sum[1] += gain * sample(1, index);
            if (++grain.age >= grain.length)
                grain.active = false;
        }
        return sum;
    }

    std::array<std::vector<float>, 2> history;
    std::array<Grain, 3> grains {};
    StretchControls targets;
    juce::SmoothedValue<float> mix;
    std::optional<double> expectedPpq;
    std::atomic<double> tail { 2 };
    std::atomic<float> shownPhase { 0 }, shownDebt { 0 }, shownRatio { 1 };
    std::atomic<bool> shownStretching { false };
    float lastRatio = 1;
    double sampleRate = 48000, tempo = 120, beatsPerBar = 4, phase = 0, phaseStep = 0, readPosition = 0, capacity = 0;
    int64_t written = 0, mask = 0;
    int maxCycleSamples = 4000, landingLength = 240, landing = 0, newest = -1;
    bool stretching = false, requestLanding = false, wasPlaying = false;
};
}
