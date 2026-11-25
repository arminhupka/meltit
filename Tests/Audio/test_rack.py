import sys
from pathlib import Path

import numpy as np
from pedalboard import load_plugin


SAMPLE_RATE = 48_000
SECOND = np.arange(SAMPLE_RATE) / SAMPLE_RATE
TWO_CHANNELS = np.tile((0.25 * np.sin(2 * np.pi * 1000 * SECOND)).astype(np.float32), (2, 4))
MODULES_OFF = dict(tape_active=False, tremolo_active=False, saturation_active=False)


def main():
    rack_path, tape_path = map(lambda p: str(Path(p).resolve()), sys.argv[1:3])

    def open_rack(**params):
        return load_plugin(rack_path, parameter_values=MODULES_OFF | params)

    def render(audio=TWO_CHANNELS, **params):
        return open_rack(**params).process(audio.copy(), SAMPLE_RATE, buffer_size=127, reset=False)

    assert np.array_equal(TWO_CHANNELS, render()), 'All modules bypassed must be transparent'
    assert np.array_equal(TWO_CHANNELS, render(tape_active=True, tremolo_active=True,
                                                 saturation_active=True, global_mix=0)), 'Mix 0% must be dry'

    expected = load_plugin(tape_path, parameter_values={'depth': 0, 'aged_color': 70})
    expected = expected.process(TWO_CHANNELS.copy(), SAMPLE_RATE, buffer_size=127, reset=False)
    assert np.array_equal(expected, render(tape_active=True, tape_depth=0, tape_aged_color=70)), 'Tape parity'

    low = render(tremolo_active=True, tremolo_sync=False, tremolo_rate_hz=3,
                 tremolo_depth=50, tremolo_stereo=False)
    assert np.array_equal(low[0], low[1])
    amplitude = np.sqrt(np.mean(low[0, SAMPLE_RATE:].reshape(-1, 48) ** 2, axis=1)) / (0.25 / np.sqrt(2))
    spectrum = np.abs(np.fft.rfft((amplitude - amplitude.mean()) * np.hanning(amplitude.size)))
    rate = np.fft.rfftfreq(amplitude.size, 0.001)[np.argmax(spectrum)]
    low_bounds = np.percentile(amplitude, [5, 95])
    assert abs(rate - 3) < .1 and .50 < low_bounds[0] < .56 and .94 < low_bounds[1] < 1, (rate, low_bounds)

    stereo = render(tremolo_active=True, tremolo_sync=True, tremolo_note='1/8', tremolo_stereo=True,
                    tremolo_depth=100, tremolo_mode='Type B')
    channels = np.sqrt(np.mean(stereo[:, SAMPLE_RATE:].reshape(2, -1, 48) ** 2, axis=-1))
    assert np.corrcoef(channels)[0, 1] < -.95
    spectrum = np.abs(np.fft.rfft((channels[0] - channels[0].mean()) * np.hanning(channels.shape[1])))
    rate = np.fft.rfftfreq(channels.shape[1], .001)[np.argmax(spectrum)]
    assert abs(rate - 4) < .1, rate

    def saturation_metrics(mode):
        result = render(saturation_active=True, saturation_drive_db=14.4, saturation_mode=mode)
        bins = 2 * np.abs(np.fft.rfft(result[0, -SAMPLE_RATE:])) / SAMPLE_RATE
        return 20 * np.log10([bins[1000] / .25, bins[3000] / bins[1000], bins[5000] / bins[1000]])

    soft, saturate = (saturation_metrics(mode) for mode in ('Soft', 'Saturate'))
    assert np.max(np.abs(soft - [4.52, -20, -32.7])) < 4.5, soft
    assert np.max(np.abs(saturate - [1.6, -12.2, -24.4])) < 2.0, saturate
    assert np.array_equal(TWO_CHANNELS, render(saturation_active=True, saturation_drive_db=0))

    input_samples = np.tile((0.38 * np.sin(2 * np.pi * 3700 * SECOND)).astype(np.float32), (2, 2))
    params = dict(tape_active=True, tape_depth=0, tape_aged_color=100, saturation_active=True,
                  saturation_drive_db=20)
    tape_first = render(input_samples, **params)
    drive_first = render(input_samples, **params, effect_order='Saturation > Tape > Tremolo')
    assert np.sqrt(np.mean((tape_first - drive_first) ** 2)) > .01
    configuration = params | dict(effect_order='Saturation > Tape > Tremolo', tremolo_active=True, global_mix=61)
    configured = open_rack(**configuration)
    recovered = open_rack()
    recovered.raw_state = configured.raw_state
    assert recovered.tape_active and recovered.tremolo_active and recovered.saturation_active
    assert str(recovered.effect_order) == 'Saturation > Tape > Tremolo'
    assert abs(float(recovered.global_mix) - 61) < .01
    configured_drive = float(configured.saturation_drive_db)
    assert abs(configured_drive - 20) < .03
    assert float(recovered.saturation_drive_db) == configured_drive

    for rate, block, channels in ((44100, 64, 1), (48000, 127, 2), (96000, 1024, 2)):
        rack = open_rack(tape_active=True, saturation_active=True, tremolo_active=True, saturation_drive_db=30.9)
        audio = np.full((channels, rate), .25, dtype=np.float32)
        wet = rack.process(audio.copy(), rate, buffer_size=block, reset=False)
        assert np.isfinite(wet).all() and wet.shape == audio.shape and not np.array_equal(audio, wet)

    print('Tape parity, transparent bypass/dry, two tremolo modes, saturation reference, reorder, state and rendering passed.')
    print(f'Saturation at 14.4 dB, fundamental gain and 3rd/5th harmonics: Soft {soft.round(1)}, Saturate {saturate.round(1)}')


if __name__ == '__main__':
    main()
