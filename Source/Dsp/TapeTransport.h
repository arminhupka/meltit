#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace rack
{
struct TransportControls
{
    bool stopped = false;   // Tape Stop held: the motor winds down and the tape halts.
    bool scrubbing = false; // A hand on the reels: the tape follows `scrubSpeed` instead of the motor.
    float scrubSpeed = 0;   // 1 is normal play, 0 holds the reel still, below 0 pulls the tape backwards.
    float stopSeconds = .8f, startSeconds = .3f;
};

// The whole plugin's output played off a tape that can be stopped, started and handled. The sound is recorded
// continuously; while the tape runs normally it passes straight through. Stopping winds the speed down so the
// pitch falls and the sound dies away; starting spins it back up and then crossfades onto the live signal, so no
// latency is left behind. A hand on the reels can hold the tape, drag it on or pull it back.
class TapeTransport
{
public:
    void prepare(double rate, int channels)
    {
        sampleRate = rate;
        channelCount = juce::jlimit(1, 2, channels);
        const auto length = static_cast<size_t>(rate * maximumSeconds) + 8;
        for (auto& line : lines)
            line.assign(length, 0.0f);
        write = 0;
        speed = 1;
        lag = 0;
        catchUp = 1;
        tone.fill(0.0f);
        published.store(1.0f, std::memory_order_relaxed);
    }

    void setControls(const TransportControls& next) { controls = next; }

    // How fast the tape is running now, for the reels on screen: 1 normal, 0 stopped, negative backwards.
    float currentSpeed() const { return published.load(std::memory_order_relaxed); }

    bool isIdle() const { return speed == 1.0 && lag == 0.0 && catchUp >= 1.0f && !controls.stopped && !controls.scrubbing; }

    void process(juce::AudioBuffer<float>& audio, int samples)
    {
        const auto channels = juce::jmin(channelCount, audio.getNumChannels());
        const auto size = lines[0].size();
        auto* const* data = audio.getArrayOfWritePointers();
        const auto step = 1.0 / sampleRate;
        for (int i = 0; i < samples; ++i)
        {
            for (int c = 0; c < 2; ++c)
                lines[static_cast<size_t>(c)][write] = data[juce::jmin(c, channels - 1)][i];

            if (!isIdle())
            {
                advanceSpeed(step);
                // Running slower than the recording, the playback head falls behind it; backwards, faster still.
                // Pulled back past the oldest recording, it reaches the leader and plays nothing.
                const auto furthest = static_cast<double>(size) - 8.0;
                const auto leader = lag + (1.0 - speed) > furthest;
                lag = juce::jlimit(0.0, furthest, lag + (1.0 - speed));

                // Back at speed after a stop: fade from the late tape onto the live signal.
                const auto running = !controls.stopped && !controls.scrubbing && speed >= 1.0;
                if (running && lag > 0.0)
                {
                    catchUp = juce::jmin(1.0f, catchUp + static_cast<float>(step / .06));
                    if (catchUp >= 1.0f)
                        lag = 0.0;
                }
                else if (lag > 0.0)
                    catchUp = 0.0f;

                auto position = static_cast<double>(write) - lag;
                if (position < 0)
                    position += static_cast<double>(size);
                // A slow tape gives a weak, dull signal from the head; a halted one gives none.
                const auto magnitude = static_cast<float>(std::abs(speed));
                const auto level = leader ? 0.0f : juce::jlimit(0.0f, 1.0f, magnitude * 2.5f);
                const auto cutoff = 1.0 - std::exp(-juce::MathConstants<double>::twoPi
                                                   * juce::jmax(60.0, 18000.0 * magnitude) / sampleRate);
                for (int c = 0; c < channels; ++c)
                {
                    const auto index = static_cast<size_t>(c);
                    const auto played = readLine(lines[index], position, lag) * level;
                    tone[index] += static_cast<float>(cutoff) * (played - tone[index]);
                    const auto live = data[c][i];
                    data[c][i] = std::lerp(tone[index], live, catchUp);
                }
            }
            write = (write + 1) % size;
        }
        published.store(static_cast<float>(speed), std::memory_order_relaxed);
    }

private:
    static constexpr double maximumSeconds = 6.0;

    void advanceSpeed(double step)
    {
        if (controls.scrubbing)
        {
            // The hand moves the tape almost directly; a little smoothing stands in for the tape's inertia.
            speed += (controls.scrubSpeed - speed) * (1.0 - std::exp(-step / .03));
            catchUp = 0.0f;
            return;
        }
        if (controls.stopped)
        {
            // The motor lets go: the speed falls away, slowly at first as the reels' momentum carries them.
            const auto fall = step / juce::jmax(.02, static_cast<double>(controls.stopSeconds));
            speed = speed > 0 ? juce::jmax(0.0, speed - fall * (.35 + 1.3 * speed)) : juce::jmin(0.0, speed + fall * 4.0);
            catchUp = 0.0f;
            return;
        }
        if (speed < 1.0)
            speed = juce::jmin(1.0, speed + step / juce::jmax(.02, static_cast<double>(controls.startSeconds)));
        else if (speed > 1.0)
            speed = juce::jmax(1.0, speed - step / .1);
    }

    // `lag` is how far `position` sits behind the newest sample. Within one sample of it the four-point read would
    // take its last point past the write head, from the oldest recording, so it falls back to a straight line.
    static float readLine(const std::vector<float>& line, double position, double lag)
    {
        const auto size = line.size();
        const auto index = static_cast<size_t>(position);
        const auto t = static_cast<float>(position - static_cast<double>(index));
        const auto at = [&line, size](size_t i) { return line[i % size]; };
        if (lag < 1.0)
            return std::lerp(at(index), at(index + 1), t);
        const auto y0 = at(index + size - 1), y1 = at(index), y2 = at(index + 1), y3 = at(index + 2);
        const auto c1 = .5f * (y2 - y0);
        const auto c2 = y0 - 2.5f * y1 + 2.0f * y2 - .5f * y3;
        const auto c3 = .5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }

    TransportControls controls;
    std::array<std::vector<float>, 2> lines;
    std::array<float, 2> tone {};
    size_t write = 0;
    double sampleRate = 48000, speed = 1, lag = 0;
    float catchUp = 1;
    int channelCount = 2;
    std::atomic<float> published { 1.0f };
};
}
