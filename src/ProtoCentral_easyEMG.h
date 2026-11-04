// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Ashwin Whitchurch, ProtoCentral Electronics
//
// Arduino library for the ProtoCentral easyEMG single-channel sEMG breakout board
//
// The easyEMG measures surface electromyography (sEMG) - the electrical activity of
// a contracting muscle. An INA333 instrumentation amplifier and two OPA2333 stages
// form a single-supply, AC-coupled front-end band-limited to roughly 20-310 Hz. The
// result is digitised by an on-board TI TLA2024 12-bit I2C ADC, read DIFFERENTIALLY
// against the board's buffered 1.65 V mid-rail so the pedestal cancels in hardware.
//
// This library is fully self-contained: the TLA2024 register logic and the RMS
// envelope detector are built in, so there are NO external library dependencies.
//
// Unlike an analog "muscle sensor", easyEMG hands you the RAW band-limited waveform.
// The envelope is computed here, in software, so you can always get back to the
// underlying signal.
//
// For research and education only - not a medical device.
//
// https://github.com/Protocentral/protocentral_easyemg_arduino

#ifndef PROTOCENTRAL_EASYEMG_H
#define PROTOCENTRAL_EASYEMG_H

#include <Arduino.h>
#include <Wire.h>

/////////////////////////////////////////////////////////////////////////////////////////
// I2C address
/////////////////////////////////////////////////////////////////////////////////////////
// The easyEMG ships with JP1 bridged 1-2, strapping the TLA2024 ADDR pin to GND -> 0x48.
// Cut the jumper and bridge 2-3 to move the board to 0x49 (two boards on one bus).
// An easyECG ships at 0x49, so the two share a bus without touching the jumper.
#define EASYEMG_I2C_ADDR_DEFAULT   0x48
#define EASYEMG_I2C_ADDR_ALT       0x49

/////////////////////////////////////////////////////////////////////////////////////////
// Conversion constants
/////////////////////////////////////////////////////////////////////////////////////////
// The ADC runs at +/-0.256 V full-scale (FSR_0_256V). The TLA2024 is 12-bit, so a
// full-scale reading is 2048 counts:
//   1 LSB = 0.256 V / 2048 = 125 uV at the ADC input
#define EASYEMG_ADC_LSB_V          0.000125f

// Analog gain of the front-end, input-referred.
//
// The product of the two stages is x51.7 (INA333 at 1 + 100k/R_G with R_G = 10k -> x11,
// then the OPA2333 stage at 1 + R14/R19 = 1 + 100k/27k = x4.7). The two filters shave a
// few percent off that even at the centre of the band.
//
// This gain is set by the onboard resistors. It is not programmable: the only way to
// change it is to change R10, R14 or R19 on the board. Relative measurements (envelope,
// %MVC, activation) do not depend on this constant at all; only absolute microvolts do.
#define EASYEMG_GAIN               51.7f

// Front-end passband, for reference in sketches and docs.
#define EASYEMG_HPF_HZ             20.0f    // C6/R13 (9.1k), 1-pole
#define EASYEMG_LPF_HZ             310.0f   // Sallen-Key, Q=0.5, actual -3 dB point

/////////////////////////////////////////////////////////////////////////////////////////
// TLA2024 registers
/////////////////////////////////////////////////////////////////////////////////////////
// Verified against TI SBAS846 (TLA2021/TLA2022/TLA2024, Nov 2017), Table 7.
//
// The operating word this library writes is 0x0AC3:
//   [15]    OS   = 0     no effect when writing during continuous conversion
//   [14:12] MUX  = 000   AINP = AIN0, AINN = AIN1. easyEMG wires AIN0 (pin 4) to the
//                        signal and AIN1 (pin 5) to the buffered mid-rail.
//   [11:9]  PGA  = 101   FSR = +/-0.256 V
//   [8]     MODE = 0     continuous conversion
//   [7:5]   DR   = 110   3300 SPS
//   [4:0]   resv = 00011 datasheet requires 03h always
//
// The register RESET value is 0x8583 - which is +/-2.048 V, 1600 SPS, and MODE = 1,
// i.e. single-shot/power-down. So begin() writing this word explicitly is not merely
// a resolution optimisation: without it the ADC never free-runs at all.
//
// Conversion register: D[11:0] occupies bits 15:4 and bits 3:0 always read 0, so the
// 12-bit result is left-justified and an arithmetic >>4 recovers the signed count.
#define EASYEMG_CONV_REG           0x00       // conversion result
#define EASYEMG_CONF_REG           0x01       // configuration

class easyEMG
{
  public:
    /////////////////////////////////////////////////////////////////////////////////////
    // Low-level ADC enums (replicated privately from the TLA20xx driver)
    /////////////////////////////////////////////////////////////////////////////////////

    // Data rate, CONF bits [7:5]
    enum DataRate {
        DR_128SPS  = 0x00,
        DR_250SPS  = 0x01,
        DR_490SPS  = 0x02,
        DR_920SPS  = 0x03,
        DR_1600SPS = 0x04,
        DR_2400SPS = 0x05,
        DR_3300SPS = 0x06,
    };

    // Full-scale range / PGA, CONF bits [11:9]
    enum FSRange {
        FSR_6_144V = 0x00,
        FSR_4_096V = 0x01,
        FSR_2_048V = 0x02,
        FSR_1_024V = 0x03,
        FSR_0_512V = 0x04,
        FSR_0_256V = 0x05,
    };

    // Input mux, CONF bits [14:12]
    enum Mux {
        MUX_AIN0_AIN1 = 0x00,
        MUX_AIN0_AIN3 = 0x01,
        MUX_AIN1_AIN3 = 0x02,
        MUX_AIN2_AIN3 = 0x03,
        MUX_AIN0_GND  = 0x04,
        MUX_AIN1_GND  = 0x05,
        MUX_AIN2_GND  = 0x06,
        MUX_AIN3_GND  = 0x07,
    };

    // Operating mode, CONF bit [8]
    enum Mode {
        MODE_CONTINUOUS  = 0,
        MODE_SINGLE_SHOT = 1,
    };

    /////////////////////////////////////////////////////////////////////////////////////
    // Construction / connection
    /////////////////////////////////////////////////////////////////////////////////////
    easyEMG(uint8_t i2cAddress = EASYEMG_I2C_ADDR_DEFAULT);

    // Calls wirePort.begin(), stores the port, and configures the ADC to the easyEMG
    // operating mode: AIN0/AIN1 differential, continuous, 3300 SPS, +/-0.256 V.
    //
    // The +/-0.256 V range is NOT optional. The ADC powers up at +/-2.048 V, where one
    // LSB is 19 uV referred to the electrodes and a resting muscle burst is a handful
    // of counts. begin() always writes the range explicitly.
    //
    // Returns true if the ADC ACKs on I2C. Does not print to Serial.
    bool begin(TwoWire &wirePort = Wire);

    // True if the ADC ACKs at its address.
    bool isConnected();

    /////////////////////////////////////////////////////////////////////////////////////
    // Reading the signal (each call takes a fresh sample)
    /////////////////////////////////////////////////////////////////////////////////////
    int16_t readRaw();          // signed 12-bit ADC counts, baseline-corrected
    int16_t readRawUncorrected();// signed 12-bit counts straight from the ADC
    float   readVoltage();      // differential ADC voltage in volts (referred to output)
    float   readMicrovolts();   // sEMG amplitude in uV at the electrodes - primary unit

    /////////////////////////////////////////////////////////////////////////////////////
    // RMS envelope
    /////////////////////////////////////////////////////////////////////////////////////
    // sEMG is a zero-mean AC signal, so a moving AVERAGE of it tends to zero and tells
    // you nothing. The meaningful "how hard is this muscle working" number is the RMS
    // over a sliding window - that is what this section computes.
    //
    // The window is in SAMPLES, not milliseconds, because the achievable sample rate
    // depends on your host: an RP2040 or ESP32 in a tight loop reaches 1600-3300 SPS,
    // while MicroPython or a Raspberry Pi manages a few hundred. Use
    // envelopeWindowMillis() to convert once you know your own loop rate.
    void    setEnvelopeWindow(uint8_t samples);   // 1..EASYEMG_MAX_ENVELOPE_WINDOW
    uint8_t getEnvelopeWindow();
    float   envelopeWindowMillis(float sampleRateHz);
    void    resetEnvelope();

    int16_t readEnvelopeRaw();  // RMS of the window, in ADC counts
    float   readEnvelope();     // RMS of the window, in uV at the electrodes

    // Last computed envelope, in uV, WITHOUT taking a new sample.
    float   lastEnvelope();

    // The most recent sample taken by ANY read - baseline-corrected counts, and the
    // same value in uV. Use these after readEnvelope()/readEnvelopeRaw() so one loop
    // iteration costs exactly one ADC conversion.
    int16_t lastRaw();
    float   lastMicrovolts();

    /////////////////////////////////////////////////////////////////////////////////////
    // Mains hum notch (optional, OFF by default)
    /////////////////////////////////////////////////////////////////////////////////////
    // The board deliberately ships no hardware notch: 50/60 Hz sits inside the
    // 20-310 Hz sEMG band, so removing it always costs real signal. That is the right
    // default for a clean recording.
    //
    // It is the wrong default when hum is so large that it swamps the muscle - which
    // is what happens on a mains-powered host, or with dried-out electrodes. Then a
    // narrow software notch is the pragmatic trade: you lose a slice of the band and
    // get a usable signal back.
    //
    // You must pass your ACTUAL sample rate, because the filter coefficients depend
    // on it and this board's rate is set by how fast your host polls, not by the ADC.
    // 04.EnvelopeRMS and 07.Diagnostics both measure it for you.
    //
    // Returns false (and leaves the notch off) if the rate is too low for the
    // requested frequency - below about 2.5x, a notch cannot be built.
    //
    // The notch applies to readRaw() and therefore to everything downstream:
    // microvolts, envelope, activation and %MVC. readRawUncorrected() stays raw.
    bool    setNotch(float mainsHz, float sampleRateHz, float q = 4.0f);
    void    disableNotch();
    bool    notchEnabled();

    /////////////////////////////////////////////////////////////////////////////////////
    // Digital high-pass (optional, OFF by default)
    /////////////////////////////////////////////////////////////////////////////////////
    // A 2nd-order Butterworth. The analog high-pass is only 1-pole, so it still passes
    // about 5% at 1 Hz, and a large electrode motion artifact can come through at several
    // times the size of the EMG. This takes that down to about 0.25% more, and keeps it
    // out of the envelope, activation and %MVC. It hides motion artifact rather than
    // curing it: a big enough artifact can still clip the ADC.
    //
    // As with the notch, pass your ACTUAL sample rate and pace your sampling.
    //
    // Returns false (and leaves the filter off) if the rate is too low for the corner.
    //
    // Applies to readRaw() and everything downstream. readRawUncorrected() stays raw.
    bool    setHighPass(float cornerHz, float sampleRateHz);
    void    disableHighPass();
    bool    highPassEnabled();

    /////////////////////////////////////////////////////////////////////////////////////
    // Baseline (zeroing)
    /////////////////////////////////////////////////////////////////////////////////////
    // The board measures differentially against its own buffered mid-rail, so the
    // resting count is already near zero - but amplifier offset leaves a few counts.
    // Call calibrateBaseline() once with the muscle relaxed and electrodes attached.
    void    calibrateBaseline(uint16_t samples = 256);
    void    setBaseline(int16_t offsetCounts);
    int16_t getBaseline();

    /////////////////////////////////////////////////////////////////////////////////////
    // Activation detection and MVC normalisation
    /////////////////////////////////////////////////////////////////////////////////////
    // Threshold is compared against the RMS envelope in uV. Both isActive() and
    // readPercentMVC() use the LAST computed envelope, so call readEnvelope() first
    // in your loop and then query these - one sample per loop, no double-reading.
    void    setActivationThreshold(float microvoltsRMS);
    float   getActivationThreshold();
    bool    isActive();

    // Maximum voluntary contraction, in uV RMS. Record it once with a hard contraction
    // (see the 05 example) and every later reading becomes a percentage of it - which
    // is how sports science and rehab actually compare across subjects and sessions.
    void    setMVC(float microvoltsRMS);
    float   getMVC();
    float   readPercentMVC();

    /////////////////////////////////////////////////////////////////////////////////////
    // Optional / advanced low-level passthroughs
    /////////////////////////////////////////////////////////////////////////////////////
    // Each clears its CONF field before setting it.
    void    setDataRate(DataRate rate);
    void    setRange(FSRange range);
    void    setMode(Mode mode);
    void    setMux(Mux mux);

    // Full-scale volts for the currently selected PGA range - used by the count->uV
    // maths, and exposed so a sketch that changes range can report correctly.
    float   fullScaleVolts();

    // Single-shot read for low-power/battery use: starts one conversion, waits for it
    // to finish, and returns the signed 12-bit count. Leaves the ADC in single-shot mode.
    int16_t readSingleShot();

    // Largest supported envelope window.
    static const uint8_t EASYEMG_MAX_ENVELOPE_WINDOW = 128;

  private:
    void     writeReg(uint8_t reg, uint16_t data);   // MSB first
    uint16_t readReg(uint8_t reg);                   // MSB first
    uint16_t confWord();                             // compose CONF from current fields
    void     applyConf();                            // write composed CONF (clear-then-set)
    float    countsToMicrovolts(float counts);
    float    applyNotch(float x);
    float    applyHighPass(float x);

    TwoWire *_wire;
    uint8_t  _i2cAddr;

    // Cached CONF fields (rewritten wholesale on every change -> no stale bits).
    uint8_t  _mux;
    uint8_t  _fsr;
    uint8_t  _mode;
    uint8_t  _dr;

    // Front-end model.
    int16_t  _baseline;

    // RMS envelope state (over baseline-corrected counts).
    int16_t  _envBuf[EASYEMG_MAX_ENVELOPE_WINDOW];
    uint8_t  _envWindow;
    uint8_t  _envCount;
    uint8_t  _envIndex;
    uint32_t _envSumSq;
    float    _lastEnvelope;
    int16_t  _lastRaw;

    // Thresholds.
    float    _threshold;
    float    _mvc;

    // Mains notch biquad (direct form II transposed).
    bool     _notchOn;
    float    _nb0, _nb1, _nb2, _na1, _na2;
    float    _nz1, _nz2;

    // Digital high-pass biquad (direct form II transposed).
    bool     _hpOn;
    float    _hb0, _hb1, _hb2, _ha1, _ha2;
    float    _hz1, _hz2;

    union I2C_data {
        uint8_t  u8_bytes[2];
        uint16_t u16_value;
    } _i2c_data;
};

#endif // PROTOCENTRAL_EASYEMG_H
