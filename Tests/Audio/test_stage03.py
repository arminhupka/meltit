import sys
from pathlib import Path

import numpy as np
from pedalboard import load_plugin


FS = 48000
NEW_BYPASSES = ('resampler_active', 'chorus_active', 'delay_active')


def render(plugin, signal, rate=FS, block=257):
    result = plugin.process(signal.copy(), rate, buffer_size=block, reset=False)
    assert result.shape == signal.shape and np.isfinite(result).all()
    return result


def main():
    old_path, new_path = [str(Path(arg).resolve()) for arg in sys.argv[1:3]]

    def open_new(**settings):
        return load_plugin(new_path, parameter_values={'tape_active': False} | settings)

    t = np.arange(4 * FS) / FS
    tone = np.tile((.25 * np.sin(2 * np.pi * 7000 * t)).astype(np.float32), (2, 1))
    levels = []
    for pre, post in ((False, False), (True, False), (False, True), (True, True)):
        result = render(open_new(resampler_active=True, resampler_rate_hz=12000,
                                 resampler_pre_filter=pre, resampler_post_filter=post), tone)
        spectrum = np.abs(np.fft.rfft(result[0, -FS:])) * 2.0 / FS
        assert int(np.argmax(spectrum[:6000])) == 5000
        levels.append(20 * np.log10(spectrum[5000] / .25))
    assert np.max(np.abs(np.array(levels) - [-2.63, -54.77, -16.85, -68.99])) < 2, levels

    impulse = np.zeros((2, 4 * FS), np.float32)
    pulse = FS // 2
    impulse[0, pulse] = .25
    for mode, first_channel in (('Standard', 0), ('PingPong', 1)):
        echo = render(open_new(delay_active=True, delay_sync=False, delay_time_ms=250, delay_feedback=50,
                               delay_saturation_db=0, delay_mix=100, delay_type=mode), impulse)
        first = pulse + FS // 4
        second = pulse + FS // 2
        first_slice = slice(first - 30, first + 100)
        second_slice = slice(second - 30, second + 100)
        assert np.max(np.abs(echo[:, :first-30])) < 1e-7
        assert np.max(np.abs(echo[first_channel, first_slice])) > .012
        assert np.max(np.abs(echo[1 - first_channel, first_slice])) < 1e-7
        second_channel = 0
        assert np.max(np.abs(echo[second_channel, second_slice])) > .004
        assert np.max(np.abs(echo[1 - second_channel, second_slice])) < 1e-7

    frozen = open_new(delay_active=True, delay_sync=False, delay_time_ms=250, delay_feedback=30,
                      delay_saturation_db=0, delay_mix=100)
    render(frozen, impulse[:, :int(.62 * FS)])
    frozen.delay_freeze = True
    tail = render(frozen, np.zeros((2, 2 * FS), np.float32))
    window = FS // 4
    recent = [float(np.linalg.norm(tail[0, i:i+window])) for i in range(window, FS * 2, window)]
    assert min(recent[-4:]) > .006 and max(recent[-4:]) / min(recent[-4:]) < 1.10, recent

    chorus_input = impulse + impulse[::-1]
    chorused = render(open_new(chorus_active=True, chorus_mix=100, chorus_depth=40, chorus_rate_hz=.25,
                               chorus_stereo=True, chorus_saturation_db=0), chorus_input)
    peak_time = np.argmax(np.abs(chorused), axis=1) - pulse
    assert np.all(np.abs(peak_time * 1000 / FS - [3.15, 9.08]) < .7), peak_time

    silence = np.zeros((2, 3 * FS), np.float32)
    noise_levels = []
    for style in ('Vinyl', 'Vinyl 2', 'Electronic Hiss', 'Tape Noise', 'Room Noise'):
        noise = render(open_new(tape_active=True, tape_depth=0, tape_noise=True,
                                tape_noise_type=style, tape_noise_gain_db=-35), silence)
        noise_levels.append(20*np.log10(np.sqrt(np.mean(noise[:, FS:]**2))))
    assert np.max(np.abs(np.array(noise_levels) - [-75.41, -77.26, -66.34, -68.07, -81.5])) < 3, noise_levels

    # Use a tone because Tape's high-pass removes DC.
    sine = np.tile((.25 * np.sin(2*np.pi*1000*np.arange(6*FS)/FS)).astype(np.float32), (2, 1))
    dropout = render(open_new(tape_active=True, tape_depth=0, tape_dropout_depth=50,
                              tape_dropout_rate_hz=2.3), sine)
    envelope = np.sqrt(np.mean(dropout[0, FS:].reshape(-1, 240)**2, axis=1)) / (.25 / np.sqrt(2))
    assert .54 < np.min(envelope) < .75 and np.max(envelope) > .98

    legacy_params = dict(tape_active=True, tape_depth=0, tape_aged_color=55, tremolo_active=True,
                         saturation_active=True, saturation_drive_db=17, global_mix=67,
                         effect_order='Saturation > Tape > Tremolo')
    legacy = load_plugin(old_path, parameter_values=legacy_params)
    upgraded = open_new(resampler_active=True, chorus_active=True, delay_active=True, tape_noise=True)
    upgraded.raw_state = legacy.raw_state
    assert not any(getattr(upgraded, name) for name in NEW_BYPASSES) and not upgraded.tape_noise
    assert np.array_equal(render(legacy, tone), render(upgraded, tone)), '0.2 audio must survive upgrade unchanged'
    reordered_input = dict(saturation_active=True, saturation_drive_db=17, resampler_active=True,
                           resampler_rate_hz=12000, resampler_pre_filter=True)
    default_order = render(open_new(**reordered_input), tone)
    different_order = render(open_new(**reordered_input, resampler_slot='1'), tone)
    assert np.sqrt(np.mean((default_order - different_order)**2)) > .001

    configured = open_new(**reordered_input, chorus_active=True, delay_active=True, resampler_slot='1',
                          chorus_slot='2', delay_slot='3', delay_feedback=127, tape_noise=True,
                          tape_noise_gain_db=-31, global_mix=67)
    recovered = open_new()
    recovered.raw_state = configured.raw_state
    assert all(getattr(recovered, name) for name in NEW_BYPASSES)
    assert [float(recovered.resampler_slot), float(recovered.chorus_slot), float(recovered.delay_slot)] == [1, 2, 3]
    assert float(recovered.delay_feedback) == float(configured.delay_feedback)
    assert recovered.tape_noise and float(recovered.tape_noise_gain_db) == float(configured.tape_noise_gain_db)
    for sample_rate, block, channels in ((44100, 64, 1), (48000, 257, 2), (96000, 1024, 2)):
        active = open_new(tape_active=True, tremolo_active=True, saturation_active=True, saturation_drive_db=14,
                          resampler_active=True, resampler_rate_hz=44, chorus_active=True, delay_active=True)
        audio = np.tile(np.asarray([.3,0,0,0], dtype=np.float32), (channels, sample_rate//4))
        assert np.max(np.abs(render(active, audio, sample_rate, block))) < 4

    print('LOFREQ filter response, chorus delay, delay timing/ping-pong/freeze, Tape noise/dropout, order and migration passed.')
    print('LOFREQ alias levels (dB)', np.round(levels, 2), 'Tape noise levels (dBFS)', np.round(noise_levels, 2))


if __name__ == '__main__':
    main()
