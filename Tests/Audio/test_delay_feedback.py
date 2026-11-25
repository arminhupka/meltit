import sys
from pathlib import Path

import numpy as np
from pedalboard import load_plugin


FS = 48_000
BASE = dict(tape_active=False, delay_active=True, delay_sync=False, delay_time_ms=25,
            delay_mix=100, delay_saturation_db=0, delay_lpf_hz=20000)


def plugin(path, **settings):
    return load_plugin(path, parameter_values=BASE | settings)


def render(effect, audio):
    return effect.process(audio.copy(), FS, buffer_size=257, reset=False)


def main():
    current, previous = [str(Path(path).resolve()) for path in sys.argv[1:3]]
    signal = np.zeros((2, 9 * FS), np.float32)
    samples = FS // 40
    burst = .12 * np.sin(2 * np.pi * 440 * np.arange(samples) / FS) * np.hanning(samples)
    signal[:, FS // 4:FS // 4 + samples] = [burst, -.5 * burst]
    settings = [
        dict(delay_feedback=97),
        dict(delay_feedback=100),
        dict(delay_feedback=200),
        dict(delay_feedback=200, delay_type="PingPong"),
        dict(delay_feedback=80, delay_saturation_db=18),
        dict(delay_feedback=200, delay_saturation_db=18, delay_lpf_slope="24 dB", delay_mod_depth=50),
        dict(delay_feedback=55, delay_saturation_db=18, lfo_1_active=True,
             lfo_1_sync=False, lfo_1_shape="Square", lfo_1_rate_hz=4,
             lfo_1_target_1="Delay Feedback", lfo_1_depth_1=100,
             macro_amount=100, macro_target_1="Delay Feedback", macro_depth_1=100),
    ]
    for options in settings:
        result = render(plugin(current, **options), signal)
        peak = np.max(np.abs(result))
        residual = np.max(np.abs(result[:, -FS:]))
        assert np.isfinite(result).all() and .02 < peak < .85 and residual < .003, (options, peak, residual)

    neutral = dict(delay_feedback=50, delay_saturation_db=0)
    assert np.array_equal(render(plugin(previous, **neutral), signal), render(plugin(current, **neutral), signal))

    # A saturated attack must not amplify later repeats relative to the first echo.
    pulse = np.zeros_like(signal)
    pulse[0, FS // 4:FS // 4 + samples // 2] = burst[:samples // 2]
    def repeat_levels(drive):
        echo = render(plugin(current, delay_feedback=80.5, delay_saturation_db=drive), pulse)[0]
        start = FS // 4
        return [np.linalg.norm(echo[start + n * samples:start + (n + 1) * samples]) for n in (1, 2, 4, 12)]
    clean = np.asarray(repeat_levels(0))
    driven = np.asarray(repeat_levels(6.78))
    assert driven[0] > 2 * clean[0]
    assert np.allclose(driven / driven[0], clean / clean[0], atol=.003), (clean, driven)

    older = plugin(previous, delay_feedback=127)
    restored = plugin(current)
    restored.raw_state = older.raw_state
    assert abs(float(restored.delay_feedback) - 127) < .01
    assert np.array_equal(render(restored, signal), render(plugin(current, delay_feedback=100), signal))

    frozen = plugin(current, delay_feedback=200)
    render(frozen, signal[:, :FS // 4 + samples])
    frozen.delay_freeze = True
    hold = render(frozen, np.zeros((2, 2 * FS), np.float32))
    levels = [np.linalg.norm(chunk) for chunk in np.split(hold[:, FS:], 4, axis=1)]
    assert min(levels) > .05 and max(levels) / min(levels) < 1.02, levels
    frozen.delay_freeze = False
    thawed = render(frozen, np.zeros_like(signal))
    assert np.isfinite(thawed).all() and np.max(np.abs(thawed[:, -FS:])) < .003
    print("Delay feedback decay, Saturation, PingPong, LFO, Macro, legacy state and Freeze passed.")


if __name__ == "__main__":
    main()
