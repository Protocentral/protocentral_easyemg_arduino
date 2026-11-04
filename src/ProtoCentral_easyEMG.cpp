// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Ashwin Whitchurch, ProtoCentral Electronics
//
// Arduino library for the ProtoCentral easyEMG single-channel sEMG breakout board
//
// https://github.com/Protocentral/protocentral_easyemg_arduino

#include "ProtoCentral_easyEMG.h"

// CONF [4:0] comparator field. The TLA2024 has no comparator; the datasheet default
// is 0b00011, which the ProtoCentral TLA20xx driver also carries.
#define EASYEMG_CONF_COMP_DEFAULT  0x03

// Full-scale volts per FSRange enum value, indexed by the PGA field.
static const float EASYEMG_FSR_VOLTS[6] = {
    6.144f,   // FSR_6_144V
    4.096f,   // FSR_4_096V
    2.048f,   // FSR_2_048V
    1.024f,   // FSR_1_024V
    0.512f,   // FSR_0_512V
    0.256f,   // FSR_0_256V
};

easyEMG::easyEMG(uint8_t i2cAddress)
{
    _wire    = &Wire;
    _i2cAddr = i2cAddress;

    // easyEMG operating mode (see begin()).
    _mux  = MUX_AIN0_AIN1;
    _fsr  = FSR_0_256V;
    _mode = MODE_CONTINUOUS;
    _dr   = DR_3300SPS;

    _baseline = 0;

    // Envelope defaults to a 64-sample window (~100 ms at a few hundred SPS).
    _envWindow    = 64;
    _envCount     = 0;
    _envIndex     = 0;
    _envSumSq     = 0;
    _lastEnvelope = 0.0f;
    _lastRaw      = 0;
    for (uint8_t i = 0; i < EASYEMG_MAX_ENVELOPE_WINDOW; i++) {
        _envBuf[i] = 0;
    }

    _threshold = 0.0f;
    _mvc       = 0.0f;

    _notchOn = false;
    _nb0 = 1.0f; _nb1 = 0.0f; _nb2 = 0.0f; _na1 = 0.0f; _na2 = 0.0f;
    _nz1 = 0.0f; _nz2 = 0.0f;

    _hpOn = false;
    _hb0 = 1.0f; _hb1 = 0.0f; _hb2 = 0.0f; _ha1 = 0.0f; _ha2 = 0.0f;
    _hz1 = 0.0f; _hz2 = 0.0f;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Low-level register I/O (MSB first), ported from the ProtoCentral TLA20xx driver.
/////////////////////////////////////////////////////////////////////////////////////////

void easyEMG::writeReg(uint8_t reg, uint16_t data)
{
    _i2c_data.u16_value = data;

    _wire->beginTransmission(_i2cAddr);
    _wire->write(reg);
    _wire->write(_i2c_data.u8_bytes[1]);   // high byte first
    _wire->write(_i2c_data.u8_bytes[0]);
    _wire->endTransmission();
}

uint16_t easyEMG::readReg(uint8_t reg)
{
    _wire->beginTransmission(_i2cAddr);
    _wire->write(reg);
    _wire->endTransmission(false);
    _wire->requestFrom(_i2cAddr, (uint8_t)2);

    _i2c_data.u8_bytes[1] = _wire->read();  // high byte first
    _i2c_data.u8_bytes[0] = _wire->read();

    return _i2c_data.u16_value;
}

// Compose the full CONF word from the cached fields. Rewriting the whole word every
// time means each field is always cleared before being set, so no stale bits survive
// a range or rate change mid-session.
uint16_t easyEMG::confWord()
{
    return (uint16_t)(((_mux  & 0x07) << 12) |
                      ((_fsr  & 0x07) << 9)  |
                      ((_mode & 0x01) << 8)  |
                      ((_dr   & 0x07) << 5)  |
                      EASYEMG_CONF_COMP_DEFAULT);
}

void easyEMG::applyConf()
{
    writeReg(EASYEMG_CONF_REG, confWord());
}

float easyEMG::fullScaleVolts()
{
    return (_fsr < 6) ? EASYEMG_FSR_VOLTS[_fsr] : EASYEMG_FSR_VOLTS[FSR_0_256V];
}

/////////////////////////////////////////////////////////////////////////////////////////
// Construction / connection
/////////////////////////////////////////////////////////////////////////////////////////

bool easyEMG::begin(TwoWire &wirePort)
{
    _wire = &wirePort;
    _wire->begin();

    // Configure the easyEMG operating mode:
    //   MUX  = AIN0/AIN1 differential (signal against the buffered 1.65 V mid-rail)
    //   PGA  = +/-0.256 V full-scale   <- required; see the header note
    //   MODE = continuous
    //   DR   = 3300 SPS
    // -> CONF word 0x0AC3 (0x8AC3 with the OS bit set).
    _mux  = MUX_AIN0_AIN1;
    _fsr  = FSR_0_256V;
    _mode = MODE_CONTINUOUS;
    _dr   = DR_3300SPS;
    applyConf();

    return isConnected();
}

bool easyEMG::isConnected()
{
    _wire->beginTransmission(_i2cAddr);
    return (_wire->endTransmission() == 0);
}

/////////////////////////////////////////////////////////////////////////////////////////
// Reading the signal
/////////////////////////////////////////////////////////////////////////////////////////

int16_t easyEMG::readRawUncorrected()
{
    // 12-bit result is left-justified in the 16-bit word; arithmetic shift
    // sign-extends the signed 12-bit count.
    int16_t v = (int16_t)readReg(EASYEMG_CONV_REG);
    v >>= 4;
    return v;
}

int16_t easyEMG::readRaw()
{
    float v = (float)(readRawUncorrected() - _baseline);

    if (_hpOn) {
        v = applyHighPass(v);
    }

    if (_notchOn) {
        v = applyNotch(v);
    }

    // Clamp to the ADC's signed 12-bit span; the notch's transient can briefly
    // overshoot and we do not want that to wrap the int16.
    if (v >  2047.0f) v =  2047.0f;
    if (v < -2048.0f) v = -2048.0f;

    _lastRaw = (int16_t)(v >= 0.0f ? (v + 0.5f) : (v - 0.5f));
    return _lastRaw;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Mains hum notch
/////////////////////////////////////////////////////////////////////////////////////////

// Standard RBJ notch biquad:
//   b = [1, -2cos(w0), 1]
//   a = [1 + alpha, -2cos(w0), 1 - alpha]
bool easyEMG::setNotch(float mainsHz, float sampleRateHz, float q)
{
    // Need real headroom above the notch frequency for the design to mean anything.
    if (mainsHz <= 0.0f || sampleRateHz < (mainsHz * 2.5f) || q <= 0.0f) {
        disableNotch();
        return false;
    }

    float w0    = 2.0f * 3.14159265358979f * mainsHz / sampleRateHz;
    float cosw0 = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);
    float a0    = 1.0f + alpha;

    _nb0 = 1.0f / a0;
    _nb1 = (-2.0f * cosw0) / a0;
    _nb2 = 1.0f / a0;
    _na1 = (-2.0f * cosw0) / a0;
    _na2 = (1.0f - alpha) / a0;

    _nz1 = 0.0f;
    _nz2 = 0.0f;
    _notchOn = true;

    resetEnvelope();
    return true;
}

void easyEMG::disableNotch()
{
    _notchOn = false;
    _nz1 = 0.0f;
    _nz2 = 0.0f;
    resetEnvelope();
}

bool easyEMG::notchEnabled()
{
    return _notchOn;
}

float easyEMG::applyNotch(float x)
{
    float y = (_nb0 * x) + _nz1;
    _nz1 = (_nb1 * x) - (_na1 * y) + _nz2;
    _nz2 = (_nb2 * x) - (_na2 * y);
    return y;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Digital high-pass
/////////////////////////////////////////////////////////////////////////////////////////

// Standard RBJ high-pass biquad at Q = 1/sqrt(2) (Butterworth):
//   b = [(1 + cos(w0)) / 2, -(1 + cos(w0)), (1 + cos(w0)) / 2]
//   a = [1 + alpha, -2cos(w0), 1 - alpha]
bool easyEMG::setHighPass(float cornerHz, float sampleRateHz)
{
    if (cornerHz <= 0.0f || sampleRateHz < (cornerHz * 2.5f)) {
        disableHighPass();
        return false;
    }

    float w0    = 2.0f * 3.14159265358979f * cornerHz / sampleRateHz;
    float cosw0 = cosf(w0);
    float alpha = sinf(w0) / (2.0f * 0.70710678f);
    float a0    = 1.0f + alpha;

    _hb0 = ((1.0f + cosw0) / 2.0f) / a0;
    _hb1 = -(1.0f + cosw0) / a0;
    _hb2 = _hb0;
    _ha1 = (-2.0f * cosw0) / a0;
    _ha2 = (1.0f - alpha) / a0;

    _hz1 = 0.0f;
    _hz2 = 0.0f;
    _hpOn = true;

    resetEnvelope();
    return true;
}

void easyEMG::disableHighPass()
{
    _hpOn = false;
    _hz1 = 0.0f;
    _hz2 = 0.0f;
    resetEnvelope();
}

bool easyEMG::highPassEnabled()
{
    return _hpOn;
}

float easyEMG::applyHighPass(float x)
{
    float y = (_hb0 * x) + _hz1;
    _hz1 = (_hb1 * x) - (_ha1 * y) + _hz2;
    _hz2 = (_hb2 * x) - (_ha2 * y);
    return y;
}

float easyEMG::readVoltage()
{
    return readRaw() * (fullScaleVolts() / 2048.0f);
}

// counts -> microvolts at the electrodes:
//   uV = counts * (FS / 2048) * 1e6 / gain
float easyEMG::countsToMicrovolts(float counts)
{
    return counts * (fullScaleVolts() / 2048.0f) * 1000000.0f / EASYEMG_GAIN;
}

float easyEMG::readMicrovolts()
{
    return countsToMicrovolts((float)readRaw());
}

/////////////////////////////////////////////////////////////////////////////////////////
// RMS envelope
/////////////////////////////////////////////////////////////////////////////////////////

void easyEMG::setEnvelopeWindow(uint8_t samples)
{
    if (samples < 1) {
        samples = 1;
    }
    if (samples > EASYEMG_MAX_ENVELOPE_WINDOW) {
        samples = EASYEMG_MAX_ENVELOPE_WINDOW;
    }
    _envWindow = samples;
    resetEnvelope();
}

uint8_t easyEMG::getEnvelopeWindow()
{
    return _envWindow;
}

float easyEMG::envelopeWindowMillis(float sampleRateHz)
{
    if (sampleRateHz <= 0.0f) {
        return 0.0f;
    }
    return (1000.0f * (float)_envWindow) / sampleRateHz;
}

void easyEMG::resetEnvelope()
{
    _envCount     = 0;
    _envIndex     = 0;
    _envSumSq     = 0;
    _lastEnvelope = 0.0f;
    for (uint8_t i = 0; i < EASYEMG_MAX_ENVELOPE_WINDOW; i++) {
        _envBuf[i] = 0;
    }
}

int16_t easyEMG::readEnvelopeRaw()
{
    int16_t sample = readRaw();
    uint32_t sq    = (uint32_t)((int32_t)sample * (int32_t)sample);

    if (_envCount < _envWindow) {
        // Window not yet full: grow the running sum of squares.
        _envSumSq += sq;
        _envBuf[_envIndex] = sample;
        _envCount++;
    } else {
        // Window full: replace the oldest sample.
        int32_t old = (int32_t)_envBuf[_envIndex];
        _envSumSq -= (uint32_t)(old * old);
        _envSumSq += sq;
        _envBuf[_envIndex] = sample;
    }

    _envIndex++;
    if (_envIndex >= _envWindow) {
        _envIndex = 0;
    }

    float meanSq = (float)_envSumSq / (float)_envCount;
    float rms    = sqrtf(meanSq);

    _lastEnvelope = countsToMicrovolts(rms);

    return (int16_t)(rms + 0.5f);
}

float easyEMG::readEnvelope()
{
    readEnvelopeRaw();
    return _lastEnvelope;
}

float easyEMG::lastEnvelope()
{
    return _lastEnvelope;
}

int16_t easyEMG::lastRaw()
{
    return _lastRaw;
}

float easyEMG::lastMicrovolts()
{
    return countsToMicrovolts((float)_lastRaw);
}

/////////////////////////////////////////////////////////////////////////////////////////
// Baseline (zeroing)
/////////////////////////////////////////////////////////////////////////////////////////

void easyEMG::calibrateBaseline(uint16_t samples)
{
    if (samples == 0) {
        return;
    }

    // Average the UNCORRECTED count so repeated calls converge instead of stacking.
    int32_t sum = 0;
    for (uint16_t i = 0; i < samples; i++) {
        sum += readRawUncorrected();
    }
    _baseline = (int16_t)(sum / (int32_t)samples);

    // The filters' state refers to the old baseline; start them clean.
    _nz1 = 0.0f;
    _nz2 = 0.0f;
    _hz1 = 0.0f;
    _hz2 = 0.0f;
    resetEnvelope();
}

void easyEMG::setBaseline(int16_t offsetCounts)
{
    _baseline = offsetCounts;
    resetEnvelope();
}

int16_t easyEMG::getBaseline()
{
    return _baseline;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Activation detection and MVC normalisation
/////////////////////////////////////////////////////////////////////////////////////////

void easyEMG::setActivationThreshold(float microvoltsRMS)
{
    _threshold = microvoltsRMS;
}

float easyEMG::getActivationThreshold()
{
    return _threshold;
}

bool easyEMG::isActive()
{
    return (_threshold > 0.0f) && (_lastEnvelope >= _threshold);
}

void easyEMG::setMVC(float microvoltsRMS)
{
    _mvc = microvoltsRMS;
}

float easyEMG::getMVC()
{
    return _mvc;
}

float easyEMG::readPercentMVC()
{
    if (_mvc <= 0.0f) {
        return 0.0f;
    }
    return (_lastEnvelope / _mvc) * 100.0f;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Optional / advanced low-level passthroughs (clear-then-set each field)
/////////////////////////////////////////////////////////////////////////////////////////

void easyEMG::setDataRate(DataRate rate)
{
    _dr = (uint8_t)rate;
    applyConf();
}

void easyEMG::setRange(FSRange range)
{
    _fsr = (uint8_t)range;
    applyConf();

    // The counts->uV scale just changed, so the accumulated envelope is no longer
    // comparable with new samples.
    resetEnvelope();
}

void easyEMG::setMode(Mode mode)
{
    _mode = (uint8_t)mode;
    applyConf();
}

void easyEMG::setMux(Mux mux)
{
    _mux = (uint8_t)mux;
    applyConf();
}

int16_t easyEMG::readSingleShot()
{
    // Enter single-shot mode and start one conversion (OS bit = CONF[15]).
    _mode = MODE_SINGLE_SHOT;
    writeReg(EASYEMG_CONF_REG, (uint16_t)(confWord() | 0x8000));

    // Wait at least one conversion period for the selected data rate, then a margin.
    static const uint16_t convMicros[] = {
        7813,  // 128 SPS
        4000,  // 250 SPS
        2041,  // 490 SPS
        1087,  // 920 SPS
        625,   // 1600 SPS
        417,   // 2400 SPS
        303,   // 3300 SPS
    };
    uint16_t wait = (_dr <= DR_3300SPS) ? convMicros[_dr] : 7813;
    delayMicroseconds(wait);
    delayMicroseconds(wait);   // safety margin

    return readRaw();
}
