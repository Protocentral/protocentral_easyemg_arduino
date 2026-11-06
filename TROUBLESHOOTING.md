# Troubleshooting easyEMG

Flat traces, noisy traces and dead traces all look alike downstream. This is the order
to work in, and the traps that produce convincing-looking wrong answers.

**Start with `07.Diagnostics`.** It answers, in order: is the ADC on the bus, did the
configuration take, is the PGA real, is the conversion register changing, and what is
the signal actually doing. Two interactive commands:

- **`s`** — shorted-input test. Bridge E1+E2+REF through 10 kΩ, no body in the loop.
  Whatever remains is the board and cable's own contribution.
- **`h`** — harmonic breakdown. Notches each mains harmonic up to 450 Hz in turn, then
  all of them together, and reports how much each removes.

---

## Reading the live table

```
peak(cts)  DC(cts)  ac_raw(uV)  ac_notch(uV)  hum%   verdict
```

| Column | What it tells you |
|---|---|
| `peak` | Full scale is ±2048. Anything near it is clipped and every other number is meaningless. |
| `DC` | Should be near zero. Large values mean a stale baseline — relax and reset. |
| `ac_raw` | AC RMS in µV at the electrodes, unfiltered |
| `ac_notch` | Same with a 2 Hz notch on every mains harmonic (`setMainsNotch()`) |
| `hum%` | How much of the RMS the notches removed |

Reference levels, µV RMS at the electrodes:

| | |
|---|---|
| board noise floor | 1.1 |
| one ADC count | 2.42 |
| good electrodes, relaxed | 5–15 |
| light contraction | ~100 |
| strong contraction | ~1000 |

---

## Symptom → cause

**Flat line at exactly zero.** The conversion register is returning a constant.
`calibrateBaseline()` averages that constant and subtracts it, so every reading becomes
exactly zero. `07.Diagnostics` phase 4 detects this directly. Check the config
read-back in phase 2, then U4's solder joints — it is an X2QFN and the hardest part on
the board to place.

**A large, rock-steady "signal" that never varies.** Check the crest factor: peak ÷ RMS
≈ 1.0 is DC, not signal (1.41 is a sine, ~3.5 is noise). It is a stale baseline. The DC
operating point moves when source impedance changes, so re-zero after every electrode
change — relax, then reset.

**Noisy trace, envelope deflection too small.** Almost always electrode interface. Run
`h`: if 50 Hz removes most of the energy the pickup is mains, and the fix is electrode
impedance — fresh Ag/AgCl pads, alcohol-wiped skin left to dry, REF pressed firmly onto
**bone**. Target `ac_raw` under 100 µV relaxed, and watch that single number while
re-placing electrodes rather than judging traces.

**Hum at 100 or 150 Hz with the host on battery.** Not the host: the room. LED drivers,
fluorescent ballasts and nearby mains wiring couple into the body and the leads, and their
energy sits on the harmonics rather than at 50 Hz, so a single 50 Hz notch leaves it all.
One bench recording had 71% of its resting power in a single 150 Hz line. Run `h` with the
lights on and off. `setMainsNotch()` (on by default in `06.OpenView`) removes it, but move
away from the source or shorten and twist the leads if you can.

**Hum that grows during a contraction.** Contracting changes the electrode–skin contact, and
any imbalance between E1 and E2 turns hum on the body into signal. The envelope and %MVC
then count the hum as effort, so an MVC recorded like this reads high and every later %MVC
reads low. Fresh pads, dry skin and leads that don't move; re-record the MVC once the hum
is under control.

**Huge signal, ~100× physiological, rails the ADC.** Check the electrodes are actually
attached. Floating inputs are biased through 1 MΩ and act as antennas; nothing
downstream of a railed input is meaningful, including the envelope.

**Slow swings that follow a contraction, with the EMG only a small ripple on top.** Skin
and electrode movement. The swings pass the 1-pole analog high-pass and the envelope and
%MVC count them as effort, so %MVC can read well over 100%. Fresh pads, leads that don't
move, and contract without moving the joint. `HIGH_PASS_HZ` in `06.OpenView` hides what's
left. If the `s` test also shows swings, it isn't the electrodes.

**Mains hum that no notch touches.** See "the notch needs paced sampling" below.

**Nothing on the bus.** Confirm 3.3 V at U4 pin 8. If powering from Qwiic only, note
that the Qwiic 3V3 pin lands on the LDO *output* — see the hardware repo's Rev B notes.

---

## Traps that produce wrong answers

Every one of these produced a misleading measurement during Rev 1 bring-up.

**1. The notch needs *paced* sampling.** `setNotch()` is only about `f/Q` wide — 12.5 Hz
at 50 Hz with the default Q=4 — and each `setMainsNotch()` notch is 2 Hz wide, so the
coefficients must be built from the true sample rate. A 10% error puts it at 45 Hz, where it attenuates mains by under 4 dB: inert,
while still removing signal at the wrong frequency. A free-running loop's *average* rate
is not accurate enough, and its jitter smears the filter further. Sample on a timed
schedule and pass that exact rate. Both `06.OpenView` and `07.Diagnostics` pace at a
fixed 1000 SPS for this reason.

**2. Let the notch settle before measuring.** A notch's time constant is about
1/(π × bandwidth): 25 ms for a Q=4 notch at 50 Hz, 160 ms for a 2 Hz notch. Measure
immediately after arming it and the transient dominates the window. `07.Diagnostics`
discards 800 samples (0.8 s) first. *Symptom: notched RMS larger than unnotched.*

**3. RMS about zero conflates DC with signal.** Compute AC RMS about the window's own
mean, and report DC separately.

**4. Envelope windows are in samples, not milliseconds.** 32 samples is 256 ms at
125 SPS and 16 ms at 2 kSPS. A sample count means nothing without the rate beside it.
sEMG envelopes want 50–200 ms. Use `envelopeWindowMillis()` once you know your rate.

**5. Streaming slower than the front end aliases.** The analog chain passes to ~310 Hz.
Sample once per packet at 125 Hz and everything above 62.5 Hz folds back in — 100 Hz
mains harmonic appears at 25 Hz, 200 Hz at 50 Hz, 80 Hz EMG at 45 Hz. It looks like
high-frequency hash and nothing downstream can undo it. Oversample, average down and
stream fast enough to keep the band, as `06.OpenView` does at 500 Hz.

**6. The OpenView profile must match the baud rate.** `06.OpenView` runs at 115200 and
needs the **easyEMG Breakout** profile in OpenView 3.4 or later. A mismatch never locks
framing and produces convincing noise on every channel.

**7. `begin()` must write the config.** The TLA2024's reset value is `0x8583` —
±2.048 V, 1600 SPS, and MODE=1, i.e. single-shot/power-down. Without an explicit write
the ADC never free-runs at all.

---

## Known-good reference values

From verified Rev 1 hardware (×20.8 gain, TLA2022), for comparison when something looks wrong. Production v2 boards use ×51.7 and a TLA2024, so treat count-based values as a guide only:

| | |
|---|---|
| Config word written | `0x0AC3`, reads back identically (bit 15 reads 0 while converting) |
| I²C address | 0x48 default, 0x49 with JP1 re-bridged |
| Max sample rate | 1958 SPS on an UNO R4 @ 400 kHz I²C |
| DC with no electrodes | ~0 counts |
| High-pass corner | 22.5 Hz measured on Rev 1 (about 20 Hz design on v2) |
| Resting peak on skin, poor electrodes | ~200 counts of ±2048 |
