import sys
from pathlib import Path

import numpy as np
from pedalboard import load_plugin


FS = 48000


def sine(hertz, length=4, amplitude=.2, rate=FS):
    return (amplitude*np.sin(2*np.pi*hertz*np.arange(round(length*rate))/rate)).astype(np.float32)


def render(plugin, signal, rate=FS, block=257):
    result = plugin.process(signal.copy(), rate, buffer_size=block, reset=False)
    assert result.shape == signal.shape and np.isfinite(result).all()
    return result


def levels(signal, bins, rate=FS):
    powers = 2*np.abs(np.fft.rfft(signal[:, -rate:], axis=1))/rate
    return powers[:, bins]


def main():
    old_path, new_path = (str(Path(name).resolve()) for name in sys.argv[1:3])

    def load(**settings):
        return load_plugin(new_path, parameter_values={'tape_active': False} | settings)

    required = {'input_gain_db','output_gain_db','global_width','global_pitch_semi','global_detune_cents','eq_mid_gain_db'}
    assert required <= load().parameters.keys(), 'Missing global controls'

    left = sine(750)
    unbalanced = np.array([left, np.zeros_like(left)])
    assert np.array_equal(render(load(), unbalanced), unbalanced)
    for width, wet, expected in [(0,100,[.5,.5]),(200,100,[1.5,-.5]),(0,50,[.75,.25]),(50,50,[.875,.125])]:
        y = render(load(global_width=width,global_mix=wet),unbalanced)
        assert np.allclose(y, np.asarray(expected)[:, None]*left, atol=.0005), (width, wet)
    assert np.array_equal(render(load(global_width=0),unbalanced[:1]),unbalanced[:1])

    all_globals = dict(input_gain_db=6,output_gain_db=7,global_width=0,eq_mid_gain_db=9,global_pitch_semi=12)
    assert np.array_equal(render(load(**all_globals,global_mix=0),unbalanced),unbalanced)
    together = render(load(input_gain_db=6,output_gain_db=-6),unbalanced)
    assert np.allclose(together,unbalanced,atol=.00035)
    sat = dict(saturation_active=True,saturation_drive_db=14.4)
    sat_signal = render(load(**sat),unbalanced)
    sat_with_output = render(load(**sat,output_gain_db=6),unbalanced)
    sat_with_input = render(load(**sat,input_gain_db=6),unbalanced)
    assert np.allclose(sat_with_output, sat_signal*10**(.3), atol=.001)
    assert np.max(np.abs(sat_with_input-sat_with_output)) > .04
    narrowed = render(load(**sat,global_width=0),unbalanced)
    assert np.allclose(narrowed[0],narrowed[1],atol=1e-7)
    assert np.allclose(2*narrowed[0],sat_signal[0],atol=2e-6)

    bins = np.array([40,80,125,250,500,1000,2000,4000,6300,8000,12000,16000])
    eq_source = np.tile(np.sum([sine(h,2,.018) for h in bins],axis=0),(2,1))
    eq_cases = [
        ('low',250,[6.06,6.38,6.91,6.90,1.83,.32,.07,.02,.01,0,0,0]),
        ('mid',1000,[.02,.08,.20,.77,2.79,5.95,2.77,.74,.28,.16,.05,.02]),
        ('high',4000,[0,0,0,.02,.07,.30,1.75,6.91,7.25,6.8,6.25,6.05]),
    ]
    eq_errors=[]
    for band,freq,expected in eq_cases:
        controls={f'eq_{band}_freq_hz':freq,f'eq_{band}_gain_db':6}
        response=20*np.log10(levels(render(load(**controls),eq_source),bins)[0]/.018)
        error=float(np.sqrt(np.mean((response-expected)**2)))
        eq_errors.append(error)
        assert error < .65, (band,error,response)
        assert np.array_equal(render(load(**controls,eq_active=False),eq_source),eq_source)
    center=np.tile(sine(1000),(2,1))
    wet=render(load(**sat,eq_mid_freq_hz=1000,eq_mid_gain_db=6),center)
    reference=render(load(**sat),center)
    assert abs(20*np.log10(levels(wet,[1000])[0,0]/levels(reference,[1000])[0,0])-6) < .2

    tone=np.array([sine(440),sine(550)])
    pitch_peaks=[]
    for shift, cents in [(-12,0),(12,0),(7,0),(0,50)]:
        node=load(global_pitch_semi=shift,global_detune_cents=cents)
        y=render(node,tone)
        f=abs(np.fft.rfft(y[:,-FS*2:]*np.hanning(FS*2),axis=1)); f[:,:180]=0
        peaks=np.argmax(f,axis=1)/2
        assert np.max(np.abs(peaks-np.array([440,550])*2**((shift+cents*.01)/12))) < 2, (shift,cents,peaks)
        assert node.reported_latency_samples == 0
        pitch_peaks.append(peaks.tolist())
    split=render(load(global_pitch_semi=12,global_mix=50),tone)
    dry_level=levels(split,[440])[0,0]
    wet_level=np.linalg.norm(levels(split,np.arange(875,886))[0])
    assert np.allclose([dry_level,wet_level],[.1,.1],atol=.018), (dry_level,wet_level)

    bursts=np.zeros((2,2*FS),np.float32)
    for start in (.25,.90):
        i=round(start*FS)
        bursts[:,i:i+FS//10]=sine(440,.1)
    shifted=render(load(global_pitch_semi=7),bursts)
    energies=np.sqrt(np.mean(shifted[0].reshape(-1,240)**2,axis=1))
    first=np.flatnonzero(energies[:140]>.025)[0]
    second=140+np.flatnonzero(energies[140:]>.025)[0]
    assert abs((second-first)*.005-.65) <= .015

    # Delay's low-pass attenuates 3 kHz more than 1.5 kHz. This distinguishes the two orders.
    pitched_input=np.tile(sine(1500),(2,1))
    echo=dict(delay_active=True,delay_mix=100,delay_sync=False,delay_time_ms=180,delay_lpf_hz=1800,
              delay_feedback=0,delay_saturation_db=0)
    post=render(load(**echo,global_pitch_semi=12),pitched_input)
    before=render(load(**echo),render(load(global_pitch_semi=12),pitched_input))
    shifted_band=np.arange(2985,3016)
    assert np.linalg.norm(levels(post,shifted_band)[0]) > 2*np.linalg.norm(levels(before,shifted_band)[0])

    automation=load()
    chunks=[]
    for shift in (0,12,-7,0):
        automation.global_pitch_semi=shift
        chunks.append(render(automation,tone[:,:FS],block=113))
    assert np.max(np.abs(np.concatenate(chunks,axis=1))) < .7

    older=load_plugin(old_path,parameter_values=dict(tape_active=True,tape_depth=0,reverse_active=True,
                      reverse_pitch_semi=0,reverse_note='1/8',global_mix=56,resampler_active=True,resampler_rate_hz=12000))
    migrated=load(**all_globals)
    migrated.raw_state=older.raw_state
    assert np.array_equal(render(older,tone),render(migrated,tone)), '0.5.0 audio changed when loading its state'

    current=load(**all_globals,eq_active=False,global_detune_cents=-24,eq_high_freq_hz=4300,eq_high_gain_db=-5)
    restored=load()
    restored.raw_state=current.raw_state
    names=[*all_globals,'eq_active','global_detune_cents','eq_high_freq_hz','eq_high_gain_db']
    assert [str(getattr(current,n)) for n in names] == [str(getattr(restored,n)) for n in names]
    for rate, chans in [(44100,1),(96000,2)]:
        signal=np.tile(sine(380,2,rate=rate),(chans,1))
        assert np.max(np.abs(render(load(global_pitch_semi=-5),signal,rate=rate,block=1024))) > .06
    print('Global gain placement, EQ, Width, Pitch, Detune, automation, state and 0.5.0 migration passed.')
    print(f'EQ RMS errors in dB {np.round(eq_errors,3)}, stereo shifted peaks {pitch_peaks}.')


if __name__ == '__main__':
    main()
