# Changelog

All notable changes to this library are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [1.1.0] - 2026-11-06

### Added
- `setMainsNotch()`: a notch on every mains harmonic up to 0.45× the sample rate, each
  the same width (2 Hz by default), and `notchSections()`. AVR builds stop at three
  harmonics (`EASYEMG_MAX_NOTCH_SECTIONS`).

### Changed
- `06.OpenView` notches every harmonic (50–450 Hz) by default instead of 50 Hz only.
  Room lighting puts most of the hum at 100 and 150 Hz even on a battery-powered host.
- `07.Diagnostics`: `h` breaks the hum down across every harmonic up to 450 Hz, plus all
  of them together; the live table's `ac_notch` uses the harmonic notch; the notch now
  settles for 800 samples.
- Notch documentation in the header, README and TROUBLESHOOTING covers room lighting and
  hum that grows during a contraction.

`setNotch()` is unchanged.

## [1.0.0] - 2026-11-04

First public release.

### Added
- Raw sEMG waveform in microvolts at the electrodes, with baseline zeroing.
- Sliding-window RMS envelope, muscle-activation detection and %MVC normalisation.
- Optional software mains notch and 2nd-order digital high-pass.
- Examples 01 to 07, including OpenView streaming and diagnostics.
