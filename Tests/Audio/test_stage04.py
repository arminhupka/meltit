import sys
from pathlib import Path

import numpy as np
from pedalboard import load_plugin


FS = 48000


def render(plugin, signal, rate=FS, block=257):
    result = plugin.process(signal.copy(), rate, buffer_size=block, reset=False)
    assert result.shape == signal.shape and np.isfinite(result).all()
    return result


def peaks(signal, at, radius=.004):
    indices = [round(FS * (at - radius)), round(FS * (at + radius))]
    return np.max(np.abs(signal[:, indices[0]:indices[1]]), axis=1)


def main():
    old, new = (str(Path(arg).resolve()) for arg in sys.argv[1:3])

    def load(**settings):
        return load_plugin(new, parameter_values={'tape_active': False} | settings)

    impulse = np.zeros((2, FS * 2), np.float32)
    impulse[0, round(.02 * FS)] = .25
    settings = dict(reverse_active=True, reverse_note='1/8', reverse_mix=100, reverse_forward=0)
    reverse = render(load(**settings), impulse)
    assert np.max(np.abs(reverse[:, :round(.472 * FS)])) < 1e-6
    main = peaks(reverse, .48)
    assert .035 < main[0] < .12 and main[1] == 0, main
    assert np.max(peaks(reverse, .52)) < .001 * main[0]
    with_forward = render(load(**(settings | {'reverse_forward': 50})), impulse)
    assert np.allclose(main, peaks(with_forward, .48), atol=1e-5)
    forward_to_reverse = peaks(with_forward, .52)[0] / main[0]
    assert .30 < forward_to_reverse < .45, forward_to_reverse
    assert .07 < peaks(with_forward, .98)[0] / main[0] < .23
    pingpong = render(load(**(settings | {'reverse_type': 'PingPong', 'reverse_forward': 50})), impulse)
    assert peaks(pingpong, .48)[0] < 1e-6 and peaks(pingpong, .48)[1] > .02
    assert peaks(pingpong, .52)[0] > .008 and peaks(pingpong, .52)[1] < .001 * main[0]

    seconds = np.arange(6 * FS) / FS
    tone = np.tile((.2 * np.sin(2 * np.pi * 440 * seconds)).astype(np.float32), (2, 1))
    pitch_peaks = []
    for shift in (-12, -7, 0, 7, 12):
        actual = render(load(**(settings | {'reverse_note':'1/2','reverse_pitch_semi': shift})), tone)
        spectrum = abs(np.fft.rfft(actual[0, 2*FS:]*np.hanning(4*FS)))
        spectrum[:400] = 0
        peak = np.argmax(spectrum) / 4
        pitch_peaks.append(float(peak))
        expected = 440 * 2 ** (shift / 12)
        assert abs(peak - expected) < 5, (shift, peak, expected)

    reverb_input = np.zeros((2, 8 * FS), np.float32)
    reverb_input[0, FS//2] = .4
    reverb = dict(reverb_active=True, reverb_mix=100, reverb_decay_s=2, reverb_early_late=50)

    def first_after(signal):
        indices = np.flatnonzero(np.max(np.abs(signal), axis=0) > 1e-4)
        assert indices.size > 0
        return (indices[0] - FS//2) / FS

    onsets = []
    for kind, expected in (('Room', 0), ('Plate', .01908), ('Hall', .03817)):
        signal = render(load(**reverb, reverb_type=kind), reverb_input)
        onset = first_after(signal)
        onsets.append(onset)
        assert abs(onset - expected) < .004, (kind, onset)
        assert np.max(np.abs(signal[1])) > .002 and not np.array_equal(signal[0], signal[1])
    plate_predelay = render(load(**reverb, reverb_type='Plate', reverb_pre_delay_ms=50), reverb_input)
    assert abs(first_after(plate_predelay) - .06908) < .005

    def t30(signal):
        impulse_response = signal[:,FS//2:]
        energy = np.cumsum(np.sum(impulse_response.astype(np.float64)**2, axis=0)[::-1])[::-1]
        db = 10*np.log10(np.maximum(energy/energy[0], 1e-20))
        return 2*(np.flatnonzero(db < -35)[0]-np.flatnonzero(db < -5)[0])/FS

    decays = []
    for decay, expected in ((.5,.487), (2,1.746), (6,5.25)):
        signal = render(load(**(reverb | {'reverb_decay_s':decay,'reverb_early_late':100})), reverb_input)
        actual = t30(signal)
        decays.append(actual)
        assert abs(actual - expected) < max(.2, expected*.30), (decay, actual, expected)

    legacy_params = dict(tape_active=True, tape_depth=0, saturation_active=True, saturation_drive_db=13,
                         resampler_active=True, resampler_rate_hz=8000, resampler_slot='2', delay_active=True,
                         delay_sync=False, delay_time_ms=200, delay_mod_depth=0, global_mix=67)
    legacy = load_plugin(old, parameter_values=legacy_params)
    upgraded = load(reverse_active=True,reverb_active=True,reverse_slot='2',reverb_slot='3')
    upgraded.raw_state = legacy.raw_state
    assert not upgraded.reverse_active and not upgraded.reverb_active
    assert float(upgraded.reverse_slot) == 7 and float(upgraded.reverb_slot) == 8
    old_audio = render(legacy, tone)
    new_audio = render(upgraded, tone)
    assert np.array_equal(old_audio, new_audio), np.max(np.abs(old_audio-new_audio))

    instance = load(**reverb,reverse_active=True, reverse_forward=59, reverb_slot='1', reverse_slot='4')
    restored = load()
    restored.raw_state = instance.raw_state
    assert restored.reverse_active and restored.reverb_active
    assert float(restored.reverb_slot) == 1 and float(restored.reverse_slot) == 4
    assert float(restored.reverse_forward) == float(instance.reverse_forward)

    for rate, block, channels in ((44100, 64, 1), (48000, 257, 2), (96000, 1024, 2)):
        all_effects = load(tape_active=True, tremolo_active=True, saturation_active=True, saturation_drive_db=12,
                           resampler_active=True, resampler_rate_hz=12000, chorus_active=True, delay_active=True,
                           reverse_active=True, reverse_note='1/16', reverb_active=True, reverb_decay_s=60)
        samples = np.arange(rate * 2) / rate
        audio = np.tile((.3 * np.sin(2*np.pi*350*samples) * (samples < .6)).astype(np.float32), (channels, 1))
        wet = render(all_effects, audio, rate, block)
        assert .001 < np.max(np.abs(wet)) < 4 and not np.array_equal(wet, audio)

    print(f'Reverse orientation, Forward repeats, PingPong and pitch passed. Pitch peaks {pitch_peaks} Hz.')
    print(f'Reverb onset modes {np.round(np.array(onsets)*1000,2)} ms, T30 {np.round(decays,2)} s. State migration passed.')


if __name__ == '__main__':
    main()
