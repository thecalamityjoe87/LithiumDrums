# Shaping and mixing

Lithium Drums mixes like a drummer's console: a channel for each drum, plus shared channels for the overhead and room mics, three FX returns and the master. This note describes what happens to a hit on its way out, how the per-hit shaping and the channel effects work, and how the Edit page and the meters get live data without breaking the audio thread's rules.

## Signal flow

```
hit ─► shaping: dynamics, pitch bend, tone, attack / hold / decay
       │
       ├─ close mics ──► the drum's channel ──┐
       │                                     ├─► EQ ─► transient ─► compressor ─► saturation ─► width ─► fader & pan ─┬─► main out (through the master)
       └─ overhead and room mics ──► their own channels ──┘                                                │   or an aux out (skips the master)
                                                                                                           └─► Room, Plate and Delay sends (pre or post fader)
each send bus ─► its effect ─► return fader & pan ─► the master bus or an aux out
master bus ─► EQ ─► glue ─► crush ─► tape ─► transient ─► limiter ─► main out
```

Every sample in a kit is an isolated recording of one hit, so a hit's close-mic signal can go to that drum's channel alone. This includes its bleed into the other close mics: a kick hit as heard by the snare mic belongs to the kick's channel. Compressing the snare channel therefore compresses snare hits only, with no kick leaking into it. The overheads and room hear every drum, so they stay shared.

A kit with no close mics at all (Salamander is one stereo pair) sends every mic to the drum channels instead, so each drum still has a channel to work on. In a kit where a drum is only on the overheads and room, its channel is silent; the Edit page and the Mixer say so.

The close mics themselves have trims (level, pan, mute and solo) under "Close mics" in the Mixer. A trim applies wherever that mic's signal goes, in every drum's channel. Muting a mic for a couple of seconds still unloads it from memory.

## Shaping each hit (Edit page, first row)

These work inside the voice, on every mic of the hit, so they change the drum everywhere: close mics, overheads and room.

- **Dynamics** widens or narrows how much velocity changes the level, on top of what the recordings already do. At +100% a velocity-1 hit is up to 30 dB quieter; at -100%, up to 30 dB louder. Velocity 127 is never changed.
- **Tune** resamples by up to ±12 semitones. **Bend** starts the pitch up to ±12 semitones away and glides it back over **Bend time**: an exponential that has 5% left at that time. While it moves, the playback rate ramps linearly in 32-sample steps. All mics still share one playback position, so they stay sample-aligned.
- **Tone** is a 12 dB/octave low cut and high cut on every mic of the hit, skipped when both are open.
- **Attack** fades the hit in over up to 200 ms. **Hold** sets how long the hit plays untouched before **Decay** starts (an exponential with a time constant from 30 ms to 4 s; at Natural, the recording's own fade). The graph draws the same curve the voice uses.
- How loud the drum is in the close, overhead and room mics is set in the Edit page's Mics card (below), and its **choke note** (the note that grabs it, e.g. from an e-kit's cymbal pad) in the menu beside Strike.

## The channel chain (the Edit page and the Mixer's inspector)

The Edit page edits a drum's compressor (in its second row, beside its Mics card) and its transient shaper and saturation (at the end of its first row). Its drum menu also lists the overhead and room channels: choosing one shows that channel's transient shaper and saturation (where a drum's zones are), its compressor, and the Mics card's whole-kit view, how loud every drum is in those mics. The Mixer's inspector has the selected channel's EQ and a Channel card with its pan, stereo width, sends and output (its fader is on its strip). Behind the EQ curve, a live spectrum shows the channel before the EQ (faint) and after it (in the accent colour). The master EQ is on the FX page.

The Edit page's Mics card sets how loud the drum is in the close, overhead and room mics, the only place those levels are set. It's drawn with the drum renders and the mic renders (`assets/mic-renders`, made by `assets/blender-mics.py`): the drum with its close mic (clipped to a drum's rim, on a low stand for the kick, on a boom stand for a cymbal or the hi-hat), the overheads hanging above it and the room pair on stands, each with the drum's level in it; a soft cone of light from each mic to the drum fades with the level. When there's room, a bar between the drum and the room mics shows how the drum's sound divides between the three. Mics the kit doesn't have, or that didn't record the drum, are dimmed, and their knobs are disabled.

Each channel runs the same four effects, in this order. Each one costs nothing while it's off, and switching any of them fades over 20 ms rather than cutting.

**EQ:** a 24 dB/octave high-pass, a low shelf, two bells and a high shelf. The filters are state-variable filters in Andrew Simper's trapezoidal form (`src/engine/dsp/Svf.h`). Their coefficients can change while audio runs through them without clicks, so knobs can be swept and automated. The curve on screen is computed from the filters' exact frequency response by the same `designEq()` the audio uses; a test checks it against sine waves run through the EQ (within 0.15 dB). A flat or switched-off EQ is bit-transparent.

**Transient shaper:** two pairs of envelope followers, compared in dB. A fast follower racing ahead of a slow one marks an attack, and a slow release lingering above a fast one marks the ring. Each difference, scaled by Attack or Sustain, becomes a gain change of up to ±12 dB. It's level-independent, so it needs no threshold.

**Compressor:** feed-forward and peak-sensing, with a 6 dB soft knee, stereo-linked. The mix control blends gains rather than signals, so parallel compression needs no extra buffer.

**Saturation:** a smooth soft-clipping curve (a rational approximation of tanh). The signal is driven into the curve and scaled back by the same amount, so quiet passages stay at their level and only the peaks are rounded off. It isn't oversampled, so heavy drive on cymbals can alias a little.

**Stereo width:** the sides (left minus right) scaled around the middle (left plus right), after the saturation and before the sends, fader and pan. 100% leaves the image as it is, 0% makes the channel mono and 200% doubles its sides; the middle never changes, so the level of a mono sum doesn't. A mono channel (a mono mic) is left alone, and at 100% the stage costs nothing.

## The returns and the master chain

Every channel sends to three returns: Room (convolution), Plate (the
algorithmic reverb, `src/engine/dsp/FdnReverb.cpp`) and Delay. Their effects
and the master bus chain are on the FX page; [fx.md](fx.md) describes them.

## Routing

Besides the stereo main out there are sixteen stereo aux outs, Aux 1 to Aux 16, which hosts leave disabled until you enable them. Every channel and FX return has an Output setting (a channel's in the Mixer's Channel card, a return's on its card on the FX page), and channels can share an aux out (all the toms on one, say). A channel on an aux out skips the master fader. If its aux out isn't enabled in the host, it plays through the main out instead.

## Live data for the UI

The audio thread never waits for the UI, and the UI never touches the audio thread's data. These carry what the UI shows:

- `MixerFeedback`: each channel's post-fader peak and compressor gain reduction, plus the returns', the master chain's and the master's peaks and gain reduction. The audio thread raises each value; the UI reads and clears it in one atomic exchange, 30 times a second.
- `SpectrumTap` (in `MixerFeedback`): for the Mixer EQ's spectrum, the selected channel's signal just before and after its EQ, in two lock-free ring buffers. The UI names the channel and says when it's looking; only then does the audio thread copy that one channel's samples (and publish the count with release ordering). The EQ graph reads the latest 2048 samples on its timer and runs the FFT on the message thread.
- `EngineFeedback`: per drum, a hit counter and the last hit (articulation, zone, round-robin and velocity), packed into one 64-bit atomic so it's always read whole. The engine stores the hit, then bumps the counter with release ordering. The Edit page polls the counter and then reads the hit, which is how its zone strip lights up what just played and follows the articulation you're playing.

The graphs (EQ, compressor curve, envelope, reverb tail) read their parameters on a timer rather than through parameter listeners, because hosts can set parameters from the audio thread, and listeners run on whichever thread that is.

## Parameters

Each piece slot `pieceN_` has the whole drum's `volume`, `tune`, `decay`, `mute`, `solo`, `send_close`, `send_overheads`, `send_room` and `choke_note`; the shaping `dynamics`, `pitch_env`, `pitch_env_time`, `tone_hp`, `tone_lp`, `attack` and `hold`; and its channel's `fader` and `pan`. Each mic slot `micN_` has `volume`, `pan`, `mute` and `solo`: a close mic's trim, or an overhead or room channel's fader.

Every piece and mic slot also has a channel: `output`, `room_send`, `reverb_send` (the Plate), `delay_send`, `send_pre`; `eq_on`, `eq_hpf`, `eq_low_freq`, `eq_low_gain`, `eq_mid1_freq`, `eq_mid1_gain`, `eq_mid1_q`, `eq_mid2_freq`, `eq_mid2_gain`, `eq_mid2_q`, `eq_high_freq`, `eq_high_gain`; `comp_on`, `comp_threshold`, `comp_ratio`, `comp_attack`, `comp_release`, `comp_makeup`, `comp_mix`; `trans_on`, `trans_attack`, `trans_sustain`; `sat_on`, `sat_drive`, `sat_mix`; `width`. A close mic's channel parameters exist but do nothing, because hosts need the same parameters in every slot.

The returns and the master chain's parameters are listed in `src/Parameters.h`; the Plate keeps the old reverb's `reverb_*` IDs.

Defaults come from the structs the DSP uses (`ChannelSettings`, `PieceParams`, `ReverbSettings`), so the parameters and the engine can't disagree about what neutral means; a test checks it. Choosing a kit from the menu puts every piece and mic parameter back to its default. The returns and the master chain keep their settings, since they aren't part of a kit.

## Cost

Measured with `--stress --full-mix`: a dense groove with up to 57 voices sounding at once, every channel running all four effects, and every hit bent and filtered. DRSKit goes from 14% of one core to 28%, and MuldjordKit from 16% to 35%. The mixer is 5–6% of that; most of the rest is the per-hit tone filter and bend, which run on every mic of every sounding hit. Everything is off by default and costs nothing until it's used.
