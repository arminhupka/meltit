#pragma once

#include "Effects.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace rack
{
struct GrainControls
{
    float size = 120;          // ms
    float density = 12;        // grain starts a second, when not synced
    float position = 20;       // ms behind the read head where grains start
    float pitch = 0;           // in steps of the chosen scale (semitones when Free)
    float pitchRandom = 0;     // 0 to 1, in the knob's own range
    float sizeRandom = 0;      // 0 to 1
    float densityRandom = 0;   // 0 to 1
    float width = .5f;         // 0 to 1: how far grains may be panned at random
    float window = 1;          // 0 to 1: the width of the window's fades, 1 is a Hann window
    float feedback = 0;        // 0 to 1, into a limiter, so 1 still stays bounded
    float highpass = 80, lowpass = 9000; // Hz, inside the feedback loop
    float offset = 0;          // ms: each further voice starts this much further back
    float toneHighpass = 20, toneLowpass = 20000; // Hz, on the cloud on its way out
    float stretchSpeed = .5f;  // 0 to 1: how fast the read head moves while stretching
    float mix = .5f;
    int scale = 1;             // see GranularCloud::scaleNames
    int voices = 8;            // 1 to 16
    int division = 10;         // see GranularCloud::divisionNames
    int retrigger = 0;         // 0 is off, else retriggerNames - 1
    bool reverse = false, sync = false, stretch = false;
};

// What the cloud is doing now, for the card's screen: one dot per grain voice.
struct GrainDot
{
    float ms = 0;        // how far behind the live input it reads
    float pitch = 0;     // semitones
    float pan = 0;       // -1 left to 1 right
    float progress = 0;  // 0 to 1 through the grain
    bool active = false;
};
using GrainPicture = std::array<GrainDot, 16>;

// A granular cloud. The input runs into a long stereo ring; a scheduler starts windowed grains from a fixed pool
// of sixteen voices that read it back from a little way behind the read head, each with its own length, pitch and
// pan. Pitches can be fitted to a scale, counted in that scale's steps. In stretch mode the read head leaves the
// live input and creeps along at its own speed, coming back to the input on a retrigger. The cloud can feed back
// into the ring through an envelope limiter, a soft clipper and a band of Butterworth filters.
class GranularCloud
{
public:
    static constexpr int voiceCount = 16;
    static constexpr double bufferSeconds = 20.0;

    static juce::StringArray scaleNames()
    {
        return { "Free", "Chromatic", "Fifths", "Octaves", "Major Pent", "Minor Pent", "Major Triad", "Suspended",
                 "Minor Triad", "Minor Hex", "Major 7th", "Minor 7th", "Dominant 7th", "Diminished", "Augmented",
                 "Augmented Hex", "Whole Tone" };
    }

    // Grain spacing: triplet, straight and dotted for each note from 1/64 to 1/2, then whole bars.
    static juce::StringArray divisionNames()
    {
        juce::StringArray names;
        for (const auto* note : { "1/64", "1/32", "1/16", "1/8", "1/4", "1/2" })
            for (const auto* feel : { "T", "", "D" })
                names.add(juce::String(note) + feel);
        for (int bars = 1; bars <= 8; ++bars)
            names.add(juce::String(bars) + (bars == 1 ? " bar" : " bars"));
        return names;
    }

    // Retrigger spacing for the stretch head, longest first: whole bars, then dotted, straight and triplet notes.
    static juce::StringArray retriggerNames()
    {
        juce::StringArray names { "Off" };
        for (int bars = 8; bars >= 1; --bars)
            names.add(juce::String(bars) + (bars == 1 ? " bar" : " bars"));
        for (const auto* note : { "1/2", "1/4", "1/8", "1/16", "1/32", "1/64" })
            for (const auto* feel : { "D", "", "T" })
                names.add(juce::String(note) + feel);
        return names;
    }

    // Beats (quarter notes) between grain starts for each division.
    static double divisionBeats(int index)
    {
        index = juce::jlimit(0, 25, index);
        if (index >= 18)
            return 4.0 * (index - 17);
        constexpr std::array bases { 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1.0, 2.0 };
        constexpr std::array feels { 1.0 / 1.5, 1.0, 1.5 };
        return bases[static_cast<size_t>(index / 3)] * feels[static_cast<size_t>(index % 3)];
    }

    // Beats between retriggers; `index` counts from the first entry after Off.
    static double retriggerBeats(int index)
    {
        index = juce::jlimit(0, 25, index);
        if (index < 8)
            return 4.0 * (8 - index);
        constexpr std::array bases { 2.0, 1.0, 1.0 / 2, 1.0 / 4, 1.0 / 8, 1.0 / 16 };
        constexpr std::array feels { 1.5, 1.0, 1.0 / 1.5 };
        index -= 8;
        return bases[static_cast<size_t>(index / 3)] * feels[static_cast<size_t>(index % 3)];
    }

    // Fits a pitch counted in steps of `scale` to semitones. Free passes it through; Fifths and Octaves count whole
    // fifths or octaves; the chord and scale modes take the nearest step and wrap it into octaves.
    static float quantise(float steps, int scale)
    {
        static constexpr std::array<std::array<int, 12>, 17> degrees {{
            {}, {{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }}, {}, {},
            {{ 0, 2, 4, 7, 9 }}, {{ 0, 3, 5, 7, 10 }}, {{ 0, 4, 7 }}, {{ 0, 2, 5, 7, 9, 11 }}, {{ 0, 3, 7 }},
            {{ 0, 3, 5, 7, 8, 10 }}, {{ 0, 4, 7, 11 }}, {{ 0, 3, 7, 10 }}, {{ 0, 4, 7, 10 }}, {{ 0, 3, 6, 9 }},
            {{ 0, 4, 8 }}, {{ 0, 3, 4, 7, 8, 11 }}, {{ 0, 2, 4, 6, 8, 10 }}
        }};
        static constexpr std::array<int, 17> counts { 0, 12, 0, 0, 5, 5, 3, 6, 3, 6, 4, 4, 4, 4, 3, 6, 6 };
        switch (scale)
        {
            case 0: return steps;
            case 2: return std::trunc(steps) * 7.0f;
            case 3: return std::trunc(steps) * 12.0f;
            default: break;
        }
        if (scale < 0 || scale >= static_cast<int>(counts.size()))
            return steps;
        const auto count = counts[static_cast<size_t>(scale)];
        const auto nearest = static_cast<int>(std::lround(steps));
        const auto octave = nearest >= 0 ? nearest / count : -((count - 1 - nearest) / count);
        const auto degree = nearest - octave * count;
        return 12.0f * static_cast<float>(octave) + static_cast<float>(degrees[static_cast<size_t>(scale)][static_cast<size_t>(degree)]);
    }

    // A Tukey window: cosine fades of `shape` of the grain shared between both ends around a flat top. A shape of 1
    // is a Hann window; small shapes approach a rectangle.
    static float window(int age, int length, float shape)
    {
        const auto last = static_cast<float>(length - 1);
        const auto edge = juce::jmax(1.0f, last * shape * .5f);
        const auto n = static_cast<float>(age);
        if (n < edge)
            return .5f - .5f * std::cos(juce::MathConstants<float>::pi * n / edge);
        if (n > last - edge)
            return .5f - .5f * std::cos(juce::MathConstants<float>::pi * (last - n) / edge);
        return 1.0f;
    }

    void prepare(double rate, GrainControls controls)
    {
        sampleRate = rate;
        length = static_cast<int>(rate * bufferSeconds);
        for (auto& line : lines)
            line.assign(static_cast<size_t>(length), 0.0f);
        writeIndex = 0;
        lag = 0;
        countdown = 0;
        retriggerCountdown = 0;
        voices = {};
        previousSum = {};
        envelope = 0;
        filters = {};
        tone = {};
        tempo = 120;
        targets = controls;
        publishTail();
        attack =std::exp(-6283.1855 / (rate * 100.0));
        release = std::exp(-6283.1855 / (rate * 1000.0));
        initialise(mix, rate, controls.mix, .005);
        initialise(feedbackGain, rate, controls.feedback, .02);
        retune();
        publish();
    }

    void beginBlock(GrainControls controls, const juce::AudioPlayHead* playHead)
    {
        setTargets(controls);
        publish();
        aligned = false;
        if (playHead == nullptr)
            return;
        if (const auto position = playHead->getPosition())
        {
            if (const auto bpm = position->getBpm(); bpm && std::isfinite(*bpm) && *bpm > 0)
                tempo = *bpm;
            const auto ppq = position->getPpqPosition();
            if (position->getIsPlaying() && ppq && std::isfinite(*ppq))
            {
                // Synced grains fall on the host's grid rather than wherever the card happened to start.
                const auto beats = divisionBeats(targets.division);
                const auto phase = *ppq / beats - std::floor(*ppq / beats);
                if (targets.sync)
                {
                    countdown = (1.0 - phase) * beats * samplesPerBeat();
                    aligned = true;
                }
                if (targets.stretch && targets.retrigger > 0)
                {
                    const auto every = retriggerBeats(targets.retrigger - 1);
                    const auto at = *ppq / every - std::floor(*ppq / every);
                    retriggerCountdown = (1.0 - at) * every * samplesPerBeat();
                }
            }
        }
    }

    void setTargets(GrainControls controls)
    {
        const auto retuned = controls.highpass != targets.highpass || controls.lowpass != targets.lowpass
                          || controls.toneHighpass != targets.toneHighpass || controls.toneLowpass != targets.toneLowpass;
        if (controls.stretch != targets.stretch)
            lag = 0;
        targets = controls;
        publishTail();
        mix.setTargetValue(controls.mix);
        feedbackGain.setTargetValue(controls.feedback);
        if (retuned)
            retune();
    }

    void process(std::array<float, 2>& samples, int channelCount, float wet)
    {
        const auto amount = wet * mix.getNextValue();
        const auto loop = feedbackGain.getNextValue();
        const auto right = static_cast<size_t>(juce::jmin(1, channelCount - 1));

        // The previous sample's cloud comes back into the ring before this sample's grains read it.
        const auto returned = feedback(previousSum, loop * wet);
        lines[0][static_cast<size_t>(writeIndex)] = samples[0] + returned[0];
        lines[1][static_cast<size_t>(writeIndex)] = samples[right] + returned[1];

        const auto busy = std::any_of(voices.begin(), voices.end(), [](const Voice& v) { return v.active; });
        if (amount <= 0.0f && !busy && previousSum == std::array<float, 2> {})
        {
            // An idle card still records, so turning it up starts from audio that is already there.
            advanceHeads();
            countdown = aligned ? countdown - 1 : 0;
            return;
        }

        if (--countdown <= 0)
        {
            start();
            countdown += interval();
        }

        std::array<float, 2> sum {};
        for (auto& voice : voices)
        {
            if (!voice.active)
                continue;
            const auto base = static_cast<int>(voice.position);
            const auto next = base + 1 == length ? 0 : base + 1;
            const auto t = static_cast<float>(voice.position - base);
            const auto gain = window(voice.age, voice.length, voice.shape);
            for (size_t c = 0; c < 2; ++c)
            {
                const auto& line = lines[c];
                const auto read = line[static_cast<size_t>(base)] * (1.0f - t) + line[static_cast<size_t>(next)] * t;
                sum[c] += read * gain * voice.pan[c];
            }
            voice.position = wrap(voice.position + voice.step);
            voice.active = ++voice.age < voice.length;
        }
        advanceHeads();
        previousSum = sum;

        const auto level = loudness();
        if (toneActive)
            for (size_t c = 0; c < 2; ++c)
                sum[c] = tone[c][1].process(tone[c][0].process(sum[c]));
        if (channelCount < 2)
        {
            samples[0] = std::lerp(samples[0], (sum[0] + sum[1]) * juce::MathConstants<float>::sqrt2 * .5f * level, amount);
            return;
        }
        samples[0] = std::lerp(samples[0], sum[0] * level, amount);
        samples[1] = std::lerp(samples[1], sum[1] * level, amount);
    }

    // Published by the audio thread whenever the controls change; the host may ask from any thread.
    double tailSeconds() const { return tail.load(std::memory_order_relaxed); }

    // The grains as they were at the start of this block; safe to read from the message thread.
    GrainPicture picture() const
    {
        GrainPicture dots;
        for (size_t i = 0; i < dots.size(); ++i)
        {
            const auto& d = display[i];
            dots[i] = { d.ms.load(std::memory_order_relaxed), d.pitch.load(std::memory_order_relaxed),
                        d.pan.load(std::memory_order_relaxed), d.progress.load(std::memory_order_relaxed),
                        d.progress.load(std::memory_order_relaxed) >= 0.0f };
        }
        return dots;
    }

private:
    struct Voice
    {
        double position = 0, step = 1;
        std::array<float, 2> pan { 1, 1 };
        float shape = 1, semitones = 0, panValue = 0;
        int age = 0, length = 0;
        bool active = false;
    };

    struct DisplayDot
    {
        std::atomic<float> ms { 0 }, pitch { 0 }, pan { 0 }, progress { -1 };
    };

    // Second-order Butterworth section, bilinear-transformed.
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

        void tune(double hertz, double rate, bool highpass)
        {
            const auto k = std::tan(juce::MathConstants<double>::pi * juce::jlimit(10.0, rate * .45, hertz) / rate);
            const auto norm = 1.0 / (1.0 + juce::MathConstants<double>::sqrt2 * k + k * k);
            b0 = static_cast<float>(highpass ? norm : k * k * norm);
            b1 = static_cast<float>(highpass ? -2.0 * norm : 2.0 * k * k * norm);
            b2 = b0;
            a1 = static_cast<float>(2.0 * (k * k - 1.0) * norm);
            a2 = static_cast<float>((1.0 - juce::MathConstants<double>::sqrt2 * k + k * k) * norm);
        }

        float process(float x)
        {
            auto y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            if (!(std::abs(y) > 1e-15f && std::abs(y) < 1e15f))
                y = 0;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            return y;
        }
    };

    double samplesPerBeat() const { return sampleRate * 60.0 / tempo; }

    float random() { return generator.nextFloat(); }

    // Randomness works in the knob's own travel: the value moves by up to `amount` of the whole range either way,
    // then stops at the ends.
    float scatter(float normalised, float amount)
    {
        return juce::jlimit(0.0f, 1.0f, normalised + juce::jlimit(0.0f, 1.0f, amount) * (2.0f * random() - 1.0f));
    }

    static float logNormal(float value, float low, float high)
    {
        return std::log(juce::jlimit(low, high, value) / low) / std::log(high / low);
    }
    static float fromLogNormal(float normal, float low, float high) { return low * std::pow(high / low, normal); }

    double interval()
    {
        if (targets.sync)
            return juce::jmax(1.0, divisionBeats(targets.division) * samplesPerBeat());
        const auto density = fromLogNormal(scatter(logNormal(targets.density, .5f, 100.0f), targets.densityRandom), .5f, 100.0f);
        return juce::jmax(1.0, sampleRate / density);
    }

    double period() const
    {
        return targets.sync ? divisionBeats(targets.division) * samplesPerBeat()
                            : sampleRate / juce::jlimit(.5, 100.0, static_cast<double>(targets.density));
    }

    void start()
    {
        const auto limit = static_cast<size_t>(juce::jlimit(1, voiceCount, targets.voices));
        size_t index = 0;
        while (index < limit && voices[index].active)
            ++index;
        if (index == limit)
            return;
        auto& voice = voices[index];

        const auto sizeMs = fromLogNormal(scatter(logNormal(targets.size, 10.0f, 1000.0f), targets.sizeRandom), 10.0f, 1000.0f);
        const auto pitchSteps = 48.0f * scatter((juce::jlimit(-24.0f, 24.0f, targets.pitch) + 24.0f) / 48.0f, targets.pitchRandom) - 24.0f;
        const auto semitones = juce::jlimit(-48.0f, 48.0f, quantise(pitchSteps, targets.scale));
        const auto step = std::exp2(static_cast<double>(semitones) / 12.0);
        const auto pan = 2.0f * scatter(.5f, targets.width) - 1.0f;
        const auto angle = pan * juce::MathConstants<float>::pi * .25f;
        const auto grainLength = juce::jmax(2, static_cast<int>(sizeMs * .001f * static_cast<float>(sampleRate)));

        // Start behind the read head; overlapping voices are spread a couple of milliseconds apart so that a dense
        // cloud of long grains does not read the same audio in phase.
        auto back = static_cast<double>(juce::jmax(0.0f, targets.position)) * .001 * sampleRate + lag + 1.0;
        if (grainLength * .5 > period())
            back += static_cast<double>(index) * .002 * sampleRate;
        back += static_cast<double>(index) * juce::jlimit(0.0f, 250.0f, targets.offset) * .001 * sampleRate;
        // A grain faster than real time catches up with the newest sample; start it far enough back not to pass it.
        if (step > 1.0 && !targets.reverse)
            back += (step - 1.0) * grainLength;
        back = juce::jmin(back, static_cast<double>(length) - 2.0 - grainLength * juce::jmax(1.0, step));

        voice.position = wrap(static_cast<double>(writeIndex) - back);
        voice.step = targets.reverse ? -step : step;
        voice.pan = { (std::cos(angle) - std::sin(angle)) * juce::MathConstants<float>::sqrt2 * .5f,
                      (std::cos(angle) + std::sin(angle)) * juce::MathConstants<float>::sqrt2 * .5f };
        voice.shape = .02f + .98f * juce::jlimit(0.0f, 1.0f, targets.window);
        voice.semitones = semitones;
        voice.panValue = pan;
        voice.age = 0;
        voice.length = grainLength;
        voice.active = true;
    }

    // Overlapping grains add up; this keeps a dense cloud near the level of a sparse one.
    float loudness() const
    {
        const auto overlap = juce::jmin(static_cast<double>(juce::jlimit(1, voiceCount, targets.voices)),
                                        static_cast<double>(targets.size) * .001 * sampleRate / juce::jmax(1.0, period()));
        const auto mean = 1.0 - .49 * juce::jlimit(0.0f, 1.0f, targets.window);
        return static_cast<float>(1.0 / std::sqrt(juce::jmax(1.0, overlap * mean)));
    }

    void advanceHeads()
    {
        writeIndex = writeIndex + 1 == length ? 0 : writeIndex + 1;
        if (!targets.stretch)
            return;
        // The read head moves at the stretch speed while the input moves at one sample a sample, so it falls behind.
        lag += 1.0 - juce::jlimit(0.0, 1.0, static_cast<double>(targets.stretchSpeed));
        const auto room = static_cast<double>(length) * .5;
        if (targets.retrigger > 0 && --retriggerCountdown <= 0)
        {
            lag = 0;
            retriggerCountdown += juce::jmax(1.0, retriggerBeats(targets.retrigger - 1) * samplesPerBeat());
        }
        if (lag > room)
            lag = 0;
    }

    std::array<float, 2> feedback(std::array<float, 2> cloud, float gain)
    {
        if (gain <= 0.0f)
        {
            envelope = 0;
            return {};
        }
        const auto magnitude = .5 * (std::abs(cloud[0]) + std::abs(cloud[1]));
        envelope = magnitude + (envelope - magnitude) * (magnitude >= envelope ? attack : release);
        constexpr double threshold = .31622776601683794; // -10 dB
        const auto limiter = envelope > threshold ? std::pow(envelope / threshold, 1.0 / 10000.0 - 1.0) : 1.0;
        std::array<float, 2> result {};
        for (size_t c = 0; c < 2; ++c)
        {
            const auto x = static_cast<float>(cloud[c] * limiter) * gain;
            const auto curved = juce::jlimit(-1.0f, 1.0f, x * (x * x + 27.0f) / (9.0f * x * x + 27.0f));
            result[c] = filters[c][1].process(filters[c][0].process(curved));
        }
        return result;
    }

    void retune()
    {
        for (auto& channel : filters)
        {
            channel[0].tune(targets.lowpass, sampleRate, false);
            channel[1].tune(targets.highpass, sampleRate, true);
        }
        // The tone filters sit out of the way at their end stops.
        toneActive = targets.toneHighpass > 20.5f || targets.toneLowpass < 19999.0f;
        for (auto& channel : tone)
        {
            channel[0].tune(targets.toneLowpass, sampleRate, false);
            channel[1].tune(targets.toneHighpass, sampleRate, true);
        }
    }

    void publishTail()
    {
        const auto reach = (targets.position + targets.size * 2.0f) * .001 + (targets.stretch ? 2.0 : 0.0);
        tail.store(targets.feedback <= .001f ? reach + .1 : juce::jmin(30.0, reach + 10.0 * targets.feedback),
                   std::memory_order_relaxed);
    }

    double wrap(double position) const
    {
        if (position >= length)
            position -= length;
        if (position < 0)
            position += length;
        return position;
    }

    void publish()
    {
        for (size_t i = 0; i < voices.size(); ++i)
        {
            const auto& v = voices[i];
            auto& d = display[i];
            if (!v.active)
            {
                d.progress.store(-1, std::memory_order_relaxed);
                continue;
            }
            auto behind = static_cast<double>(writeIndex) - v.position;
            if (behind < 0)
                behind += length;
            d.ms.store(static_cast<float>(behind * 1000.0 / sampleRate), std::memory_order_relaxed);
            d.pitch.store(v.semitones, std::memory_order_relaxed);
            d.pan.store(v.panValue, std::memory_order_relaxed);
            d.progress.store(static_cast<float>(v.age) / static_cast<float>(juce::jmax(1, v.length)), std::memory_order_relaxed);
        }
    }

    double sampleRate = 48000, tempo = 120, lag = 0, countdown = 0, retriggerCountdown = 0;
    double envelope = 0, attack = 0, release = 0;
    int length = 1, writeIndex = 0;
    bool aligned = false;
    std::array<std::vector<float>, 2> lines;
    std::array<Voice, voiceCount> voices {};
    std::array<DisplayDot, voiceCount> display;
    std::array<std::array<Biquad, 2>, 2> filters {}, tone {};
    bool toneActive = false;
    std::array<float, 2> previousSum {};
    GrainControls targets;
    std::atomic<double> tail { .34 };
    juce::SmoothedValue<float> mix, feedbackGain;
    juce::Random generator;
};
}
