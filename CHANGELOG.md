# Changelog

All notable changes to this library are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [1.1.0] - 2026-11-06

### Added
- `setMainsNotch()`: a notch on every mains harmonic up to 0.45× the sample rate, 6 Hz
  wide at the fundamental and 2 Hz on the harmonics by default, and `notchSections()`.
  The wider fundamental settles in about 0.1 s, so a jump in 50/60 Hz pickup no longer
  leaves a 0.4 s burst. AVR builds stop at three harmonics (`EASYEMG_MAX_NOTCH_SECTIONS`).
- `06.OpenView`: the host can send `m` to record the MVC again (OpenView 3.4's
  **Record MVC** button), and byte 6 of each packet reports the MVC step: 0 none,
  1 relax, 2 contract, 3 too weak, 4 set.

### Changed
- `06.OpenView` notches every harmonic (50–450 Hz) by default instead of 50 Hz only.
  Room lighting puts most of the hum at 100 and 150 Hz even on a battery-powered host.
- `07.Diagnostics`: `h` breaks the hum down across every harmonic up to 450 Hz, plus all
  of them together; the live table's `ac_notch` uses the harmonic notch; the notch now
  settles for 800 samples.
- `06.OpenView` MVC: taken from the smoothed envelope that OpenView plots, after a 2 s
  relax that measures the resting level. An MVC under 100 µV or under 5× rest is refused
  (the LED blinks) instead of making later readings several hundred percent. With
  `MVC_AUTO_RAISE`, a stronger contraction becomes the new MVC, so %MVC stays at or below
  100. %MVC is rounded rather than truncated.
- Notch documentation in the header, README and TROUBLESHOOTING covers room lighting,
  hum that grows during a contraction, 50 Hz bursts and %MVC above 100.

`setNotch()` is unchanged.

## [1.0.0] - 2026-11-04

First public release.

### Added
- Raw sEMG waveform in microvolts at the electrodes, with baseline zeroing.
- Sliding-window RMS envelope, muscle-activation detection and %MVC normalisation.
- Optional software mains notch and 2nd-order digital high-pass.
- Examples 01 to 07, including OpenView streaming and diagnostics.
