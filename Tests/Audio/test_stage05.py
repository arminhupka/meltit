import sys
from pathlib import Path

import numpy as np
from pedalboard import load_plugin


RATE = 48000


def render(plugin, signal, block=257):
    result = plugin.process(signal.copy(), RATE, buffer_size=block, reset=False)
    assert result.shape == signal.shape and np.isfinite(result).all()
    return result


def main():
    old_path, new_path = (str(Path(name).resolve()) for name in sys.argv[1:3])

    def load(**kwargs):
        return load_plugin(new_path, parameter_values={'tape_active': False} | kwargs)

    product = load()
    expected = {'lfo_1_target_1', 'lfo_2_target_1', 'macro_target_4', 'delay_drift', 'delay_unstable'}
    assert expected <= product.parameters.keys(), f'Missing modulation controls: {expected - product.parameters.keys()}'

    tone = np.tile((.25*np.sin(2*np.pi*1000*np.arange(RATE*5)/RATE)).astype(np.float32), (2, 1))
    sat = dict(saturation_active=True, saturation_drive_db=14.4, saturation_mix=50)
    lfo = dict(lfo_1_sync=False, lfo_1_rate_hz=2, lfo_1_target_1='Saturation Mix')

    def envelope(result):
        return np.sqrt(np.mean(result[0,RATE:].reshape(-1,48)**2,axis=1))

    plus = envelope(render(load(**sat, **lfo, lfo_1_depth_1=100), tone))
    minus = envelope(render(load(**sat, **lfo, lfo_1_depth_1=-100), tone))
    all_freqs = np.fft.rfftfreq(len(plus), .001)
    spectrum = np.abs(np.fft.rfft((plus - plus.mean())*np.hanning(len(plus))))
    assert all_freqs[np.argmax(spectrum)] == 2
    assert np.corrcoef(plus, minus)[0,1] < -.98
    assert np.std(plus) > .003
    block64 = envelope(render(load(**sat, **lfo, lfo_1_depth_1=100), tone, block=64))
    assert np.max(np.abs(block64-plus)) < .001

    other = dict(lfo_2_sync=True, lfo_2_division='1/2', lfo_2_target_1='Saturation Mix', lfo_2_depth_1=75)
    synced = envelope(render(load(**sat, **other), tone))
    spectrum = np.abs(np.fft.rfft((synced - synced.mean())*np.hanning(len(synced))))
    assert all_freqs[np.argmax(spectrum)] == 1, all_freqs[np.argmax(spectrum)]

    macro = dict(macro_target_1='Saturation Mix', macro_depth_1=-100, macro_target_4='Tremolo Depth', macro_depth_4=60)
    neutral = render(load(**sat, **macro, macro_amount=50), tone)
    assert np.array_equal(neutral, render(load(**sat), tone))
    assert np.allclose(render(load(**sat, **macro, macro_amount=100), tone), tone, rtol=0, atol=1e-6)
    fourth = dict(tremolo_active=True, tremolo_sync=False, tremolo_rate_hz=2, tremolo_depth=0,
                  macro_target_4='Tremolo Depth', macro_depth_4=100, macro_amount=100)
    assert np.std(envelope(render(load(**fourth), tone))) > .02
    assert np.array_equal(render(load(**fourth, macro_active_4=False), tone), tone)

    pitch_t = np.arange(RATE*18)/RATE
    carrier = np.exp(2j*np.pi*440*pitch_t)
    probe = np.tile((.25*carrier.imag).astype(np.float32), (2, 1))
    window = np.hanning(960)

    def motion(plugin):
        data = render(plugin, probe)[0]
        phasor = np.lib.stride_tricks.sliding_window_view(data*carrier.conj(),960)[::240]@window
        phase = np.unwrap(np.angle(phasor[400:]))
        return 1200*np.log2((440 + np.diff(phase)/(2*np.pi*.005))/440)

    delay = dict(delay_active=True, delay_sync=False, delay_time_ms=200, delay_feedback=0,
                 delay_saturation_db=0, delay_mix=100)
    baseline, disabled, drifting, unstable = [motion(load(**delay, **config)) for config in
        ({}, {'delay_drift':50,'global_drift':0}, {'delay_drift':50}, {'delay_unstable':50})]
    assert np.array_equal(baseline, disabled)
    low, high = np.percentile(drifting,[5,95])
    assert -16 < low < -6 and 6 < high < 16, (low, high)
    assert np.max(np.abs(unstable)) > 4 and np.mean(np.abs(unstable)<.15) > .35

    before = load_plugin(old_path, parameter_values=dict(tape_active=True,tape_depth=0,chorus_active=True,
                        delay_active=True,delay_sync=False,delay_time_ms=142,delay_saturation_db=0))
    configured = load(**sat, **lfo, **other, **macro, lfo_1_depth_1=65, macro_amount=73, delay_drift=50)
    restored = load()
    restored.raw_state = configured.raw_state
    for parameter in ('lfo_1_target_1','lfo_1_depth_1','lfo_2_division','lfo_2_target_1',
                      'macro_target_4','macro_depth_4','macro_amount','delay_drift'):
        assert str(getattr(configured,parameter)) == str(getattr(restored,parameter)), parameter

    restored.raw_state = before.raw_state
    assert restored.lfo_1_target_1 == 'None' and restored.macro_target_4 == 'None' and float(restored.delay_drift)==0
    assert np.array_equal(render(before,tone),render(restored,tone)), 'Neutral state changed 0.4 audio'

    for rate in (44100, 48000, 96000):
        time = np.arange(rate*5)/rate
        multi = np.tile((.16*sum(np.sin(2*np.pi*f*time) for f in (110,1000,6300))).astype(np.float32),(2,1))
        for shape in ('Sine','Square','Random 3'):
            extreme = load(resampler_active=True,resampler_rate_hz=22050,lfo_1_sync=False,lfo_1_rate_hz=20,
                           lfo_1_shape=shape,lfo_1_target_1='Resampler Rate',lfo_1_depth_1=100)
            processed = extreme.process(multi.copy(),rate,buffer_size=173,reset=False)
            peak = np.max(np.abs(processed))
            assert np.isfinite(processed).all() and peak < 2, (rate, shape, peak)
    print(f'LFO positive/negative, sync, macro routes, block sizes and 0.4 migration passed. Delay Drift p05 / p95 {low:.2f}/{high:.2f} cents.')


if __name__ == '__main__':
    main()
