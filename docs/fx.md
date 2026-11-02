# The FX page

Every mixer channel has three sends: **Room**, **Plate** and **Delay**. They
come back on three return strips next to the master on the Mixer page (each
with its fader, mute, pan and output), and the effects themselves are on the
FX tab. A channel's sends are after its fader unless its **Pre** switch is on.
A send's name, on a strip or in the inspector, opens its effect on the FX page;
so does clicking a return strip.

After the returns, the master bus runs a fixed chain: **EQ, glue compressor,
crush, tape, transient shaper, limiter**. The master fader feeds the chain, so
the limiter's ceiling is the last word on the main out. Channels and returns
routed to an aux out skip the master fader and chain, like a direct out.

Each module has an On switch and a preset menu of factory settings. **FX
Bypass** silences the returns and passes the master bus through the chain
untouched. **A/B** keeps two settings of the whole FX page: switching keeps
the one you leave, and Copy puts the live one into the other slot. The A/B
slots are saved with the project but not in presets; the FX settings
themselves are saved in presets like every other parameter.

## The returns

**Room** convolves the send with an impulse response, using
`juce::dsp::Convolution` in its non-uniform mode (the first 512 samples of the
IR in small blocks with no latency, the tail in bigger, cheaper ones). IRs are
decoded, normalised by energy and trimmed on a thread of their own
(`engine/IrLoader`), then handed to the Convolution, which prepares them on its
own background thread and swaps them in lock-free. **Decay** below 100% adds an
extra exponential decay, so the IR's decay time is that share of the original,
and cuts it where it has fallen 80 dB. **Load** plays your own WAV, FLAC or
AIFF; its path is kept in the parameters' state, so presets and projects keep
it. Pre-delay, low cut and high cut work on the send, width on the result.

**Plate** is the algorithmic reverb that was the mixer's only reverb before
the FX page: a feedback delay network of eight modulated lines, mixed by a
Hadamard matrix and damped so the tail falls 60 dB in the decay time. It keeps
that reverb's parameter IDs (`reverb_*` and each channel's `reverb_send`), so
older projects and presets come back with their sends and settings as they
were; parameters added since load at their defaults.

**Delay** syncs to the host's tempo (straight, dotted and triplet values from
1/1 to 1/32), or runs free in milliseconds. The send and every repeat pass the
low and high cut on their way into the line, so repeats thin out and darken;
what comes back round is soft-clipped, so high feedback saturates rather than
running away. Ping-pong sums the send to mono and bounces the repeats.

Each return has a **Mix**: at 100% it returns only the effect; lower lets some
of the send through dry.

## The master chain

| Stage | What it does |
|---|---|
| EQ | Low shelf, two bells, high shelf (and a high-pass), the channels' EQ on the master bus. |
| Glue | The channels' compressor with bus settings: threshold, ratio, attack, release, makeup, mix. |
| Crush | A copy of the bus through a 20:1 compressor pushed down by Drive, most of its level made up, blended in underneath. |
| Tape | An off-centre soft clipper at 4x oversampling (`juce::dsp::Oversampling`, polyphase IIR), then a tone tilt and output. |
| Transient | The channels' transient shaper: attack and sustain. |
| Limiter | A brickwall at the ceiling, with release, and optional 2 ms lookahead. |

The tape stage's IIR filters shift its phase very slightly, more in the highs.
That isn't a fixed delay a host could compensate, so it isn't reported.

## Latency

With the limiter's lookahead on, the plugin reports 2 ms (96 samples at
48 kHz) to the host, which then delays the other tracks to match. It's reported
whether or not the limiter is switched on, so bypassing it doesn't make the host
re-align mid-session, and the aux outs are delayed by the same amount so they
stay in time with the main out. With lookahead off, the latency is 0. Latency
is reported from the message thread, never the audio thread.

## CPU

`LithiumDrumsTests --fx-cost` times each effect alone and all together at
48 kHz with 256-sample blocks. On the development machine, with the longest
built-in IR (the 4 s Hall):

| | Share of one core |
|---|---|
| Room | 3.8% |
| Plate | 1.1% |
| Delay | 0.1% |
| The whole master chain | under 0.5% (tape 0.26%, the rest under 0.1% each) |
| Everything on | 4.7% |

A return that's muted or turned down, or that hasn't been sent anything for
longer than its tail lasts, doesn't run at all.

## The impulse responses

| IR | Source | Licence |
|---|---|---|
| Studio | Synthesized (`tools/irgen`) | CC0 |
| Seminar Room | HOMULA-RIR, Politecnico di Milano (Miotello, Ostan, Pezzoli, Comanducci, Bernardini, Antonacci, Sarti), [doi:10.5281/zenodo.10479726](https://zenodo.org/records/10479726) | CC BY 4.0 |
| Tiled Room | Synthesized | CC0 |
| Hall | Synthesized | CC0 |
| Plate | Synthesized | CC0 |

The Seminar Room is two microphones of the dataset's linear array (0 and 33,
symmetric about source S1) made into a stereo pair, trimmed, level-matched,
with its direct sound lowered by 9 dB. The About dialog credits it, with
those changes. The synthesized IRs are image-source early reflections in a
shoebox room plus a noise tail that decays at its own rate in four bands.
`tools/irgen` makes them, converts measured IRs, and `--info` measures them.
The list and credits are in `assets/irs/irs.json`.
