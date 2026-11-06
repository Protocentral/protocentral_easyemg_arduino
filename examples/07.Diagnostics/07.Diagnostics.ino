// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Ashwin Whitchurch, ProtoCentral Electronics
//
// 07.Diagnostics
//
// Run this when something is wrong and you want facts instead of guesses. It
// answers, in order, the questions that every other symptom depends on:
//
//   1. Is the ADC on the bus, and at which address?
//   2. Did our configuration actually take? (reads the CONFIG register back)
//   3. Does the TLA2024 on this board really support the +/-0.256 V PGA range?
//   4. Is the conversion register CHANGING at all, or returning a constant?
//   5. What is the signal doing - noise floor, hum, or muscle?
//
// Question 4 is the important one. A constant ADC reading makes the whole library
// report exactly zero, because calibrateBaseline() averages that constant and then
// subtracts it from every later sample. Flat traces in OpenView or the plotter look
// identical whether the board is silent or the ADC is stuck - this tells them apart.
//
// Open the Serial Monitor at 115200. No electrodes needed for phases 1-4.
//
// For research and education only - not a medical device. Power the host from a battery
// or use an isolated Qwiic adapter when electrodes are on a person.

#include <Wire.h>
#include "ProtoCentral_easyEMG.h"

easyEMG emg;

uint8_t foundAddr = 0;

// ---- helpers ---------------------------------------------------------------------

void printHex16(uint16_t v)
{
    Serial.print("0x");
    for (int8_t i = 12; i >= 0; i -= 4) {
        Serial.print((v >> i) & 0x0F, HEX);
    }
}

uint16_t readConfigRaw(uint8_t addr)
{
    Wire.beginTransmission(addr);
    Wire.write(0x01);                 // CONFIG register
    Wire.endTransmission(false);
    Wire.requestFrom(addr, (uint8_t)2);
    if (Wire.available() < 2) {
        return 0xDEAD;
    }
    uint16_t hi = Wire.read();
    uint16_t lo = Wire.read();
    return (uint16_t)((hi << 8) | lo);
}

void writeConfigRaw(uint8_t addr, uint16_t v)
{
    Wire.beginTransmission(addr);
    Wire.write(0x01);
    Wire.write((uint8_t)(v >> 8));
    Wire.write((uint8_t)(v & 0xFF));
    Wire.endTransmission();
}

// ---- phases ----------------------------------------------------------------------

void phase1_scan()
{
    Serial.println(F("--- 1. I2C scan -----------------------------------------"));
    uint8_t n = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.print(F("   device at 0x"));
            Serial.print(a, HEX);
            if (a == 0x48) { Serial.print(F("  <- easyEMG, JP1 default")); foundAddr = a; }
            if (a == 0x49) { Serial.print(F("  <- easyEMG, JP1 alternate")); if (!foundAddr) foundAddr = a; }
            Serial.println();
            n++;
        }
    }
    if (n == 0) {
        Serial.println(F("   NOTHING FOUND."));
        Serial.println(F("   Check 3V3 and GND, then SDA/SCL. If the board is powered"));
        Serial.println(F("   only from Qwiic, also confirm the 3V3 rail is really 3.3 V."));
    }
    Serial.println();
}

void phase2_config()
{
    Serial.println(F("--- 2. CONFIG register read-back ------------------------"));

    // What the library writes: MUX=AIN0/AIN1, PGA=+/-0.256V, continuous, 3300 SPS.
    const uint16_t want = 0x0AC3;

    writeConfigRaw(foundAddr, want);
    delay(10);
    uint16_t got = readConfigRaw(foundAddr);

    Serial.print(F("   wrote     ")); printHex16(want); Serial.println();
    Serial.print(F("   read back ")); printHex16(got);  Serial.println();

    if (got == 0xDEAD) {
        Serial.println(F("   READ FAILED - the ADC ACKs its address but will not return data."));
        Serial.println();
        return;
    }

    // Bit 15 (OS) reads as 1 when idle, so ignore it in the comparison.
    if ((got & 0x7FFF) == (want & 0x7FFF)) {
        Serial.println(F("   MATCH - configuration took."));
    } else {
        Serial.println(F("   MISMATCH - the part did not accept every field:"));
        uint16_t diff = (got ^ want) & 0x7FFF;
        if (diff & 0x7000) Serial.println(F("     MUX  differs (expected on a part with no input mux)"));
        if (diff & 0x0E00) Serial.println(F("     PGA  differs  <- SIGNIFICANT, see phase 3"));
        if (diff & 0x0100) Serial.println(F("     MODE differs  <- SIGNIFICANT, may be stuck single-shot"));
        if (diff & 0x00E0) Serial.println(F("     DR   differs"));
        if (diff & 0x001F) Serial.println(F("     COMP differs (harmless, no comparator on this part)"));
    }
    Serial.println();
}

void phase3_pga()
{
    Serial.println(F("--- 3. Does the PGA field exist? ------------------------"));
    Serial.println(F("   Writing each range and reading the field back. If the value"));
    Serial.println(F("   never changes, this part has a FIXED range and the library's"));
    Serial.println(F("   counts-to-microvolts scaling is wrong."));

    const char *names[6] = {"6.144", "4.096", "2.048", "1.024", "0.512", "0.256"};
    uint8_t distinct = 0;
    uint16_t seen[6];

    for (uint8_t pga = 0; pga < 6; pga++) {
        uint16_t w = (uint16_t)((pga << 9) | 0x00C3);   // MUX=0, continuous, 3300 SPS
        writeConfigRaw(foundAddr, w);
        delay(10);
        uint16_t r = readConfigRaw(foundAddr);
        uint16_t field = (r >> 9) & 0x07;

        Serial.print(F("   +/-")); Serial.print(names[pga]);
        Serial.print(F(" V -> field reads ")); Serial.print(field);
        Serial.println(field == pga ? F("  ok") : F("  NOT ACCEPTED"));

        bool isNew = true;
        for (uint8_t i = 0; i < distinct; i++) {
            if (seen[i] == field) { isNew = false; break; }
        }
        if (isNew) { seen[distinct++] = field; }
    }

    if (distinct <= 1) {
        Serial.println(F("   VERDICT: PGA field is fixed. This is not a TLA20xx."));
    } else {
        Serial.println(F("   VERDICT: PGA is programmable, as expected."));
    }

    // Leave the part in the library's operating mode.
    writeConfigRaw(foundAddr, 0x0AC3);
    delay(10);
    Serial.println();
}

void phase4_alive()
{
    Serial.println(F("--- 4. Is the conversion register changing? -------------"));
    Serial.println(F("   Taking 200 raw samples with NO baseline subtraction."));

    int16_t mn = 32767, mx = -32768;
    long sum = 0;
    for (uint16_t i = 0; i < 200; i++) {
        int16_t v = emg.readRawUncorrected();
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum += v;
        delay(2);
    }

    Serial.print(F("   min ")); Serial.print(mn);
    Serial.print(F("   max ")); Serial.print(mx);
    Serial.print(F("   mean ")); Serial.print((float)sum / 200.0f, 1);
    Serial.print(F("   spread ")); Serial.println(mx - mn);

    if (mx == mn) {
        Serial.println(F("   STUCK - every sample identical. The ADC is not converting."));
        Serial.println(F("   This is why the plotter and OpenView show flat lines: the"));
        Serial.println(F("   library averages this constant as the baseline and then"));
        Serial.println(F("   subtracts it, so every reading becomes exactly zero."));
        Serial.println(F("   Check phase 2/3 above, then the ADC solder joints (U4)."));
    } else {
        Serial.println(F("   ALIVE - the conversion register is moving."));
    }
    Serial.println();
}

// Mains frequency for the hum measurement. 50 for most of the world, 60 for the
// Americas, Japan and a few others.
const float MAINS_HZ = 50.0f;

// Width of each notch. The live table notches every harmonic up to 450 Hz at this
// width (setMainsNotch), as 06.OpenView does.
const float NOTCH_BW_HZ = 2.0f;

// Width of the single notch the 'h' breakdown puts on each harmonic in turn. A little
// wider than NOTCH_BW_HZ, so a drifting mains frequency still lands inside it.
const float BREAKDOWN_BW_HZ = 4.0f;

const uint16_t WINDOW_N = 300;   // samples averaged per reported figure
const uint16_t SETTLE_N = 800;   // samples discarded after arming a notch

// Sampling is PACED, not free-running. A 2 Hz notch at 450 Hz has to land within a
// fraction of a hertz, so its coefficients must be built from the true rate: a 1%
// error moves it 4.5 Hz, clean off the hum, while it still removes signal where it
// sits. A free-running loop's average rate is neither accurate nor uniform enough
// for that, so every measurement here is taken on a fixed 1000 SPS schedule and the
// notch is armed from that exact figure.
const long          ADC_RATE_HZ     = 1000L;
const unsigned long ADC_INTERVAL_US = 1000000L / ADC_RATE_HZ;

unsigned long nextSampleUs = 0;

// One ADC read, on schedule.
int16_t timedRead()
{
    while ((long)(micros() - nextSampleUs) < 0) { /* wait */ }
    nextSampleUs += ADC_INTERVAL_US;
    return emg.readRaw();
}

// AC RMS over one window, in uV, measured about the window's OWN mean.
//
// Reporting RMS about zero conflates a DC offset with real signal: a stale baseline
// then reads as a large, rock-steady "signal" whose peak equals its RMS. Removing the
// window mean makes the measurement immune to that, and the DC term is reported
// separately so a drifting operating point is visible rather than disguised.
float windowAcMicrovolts(int16_t *outPeakCounts, float *outDcCounts)
{
    int16_t mn = 32767, mx = -32768;
    float   sum = 0.0f, sumSq = 0.0f;

    for (uint16_t i = 0; i < WINDOW_N; i++) {
        int16_t v = timedRead();
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum   += (float)v;
        sumSq += (float)v * (float)v;
    }

    float mean = sum / (float)WINDOW_N;
    float var  = (sumSq / (float)WINDOW_N) - (mean * mean);
    if (var < 0.0f) var = 0.0f;              // rounding guard

    if (outPeakCounts) {
        int16_t a = (mn < 0) ? (int16_t)(-mn) : mn;
        *outPeakCounts = (mx > a) ? mx : a;
    }
    if (outDcCounts) *outDcCounts = mean;

    return sqrtf(var) * (emg.fullScaleVolts() / 2048.0f) * 1000000.0f / EASYEMG_GAIN;
}

// Run the notch long enough for its transient to die before anything is measured.
// A 2 Hz notch has a time constant of about 1/(pi x 2 Hz) = 160 ms, so a window
// measured straight after arming it is mostly settling, not signal. 800 samples at
// 1000 SPS is five time constants.
void settleNotch()
{
    for (uint16_t i = 0; i < SETTLE_N; i++) {
        timedRead();
    }
}

// AC RMS with no notch.
float measureRaw(int16_t *peak, float *dc)
{
    emg.disableNotch();
    return windowAcMicrovolts(peak, dc);
}

// AC RMS with ONE notch at hz, BREAKDOWN_BW_HZ wide, settled first.
//
// setNotch() takes a Q, and its design narrows towards Nyquist: Q = hz / bandwidth
// would make the 450 Hz notch about a tenth as wide as the 50 Hz one. This Q gives
// the same -3 dB width at every frequency, matching setMainsNotch().
float measureWithNotch(float hz)
{
    const float pi = 3.14159265358979f;
    float w0 = 2.0f * pi * hz / (float)ADC_RATE_HZ;
    float q  = sinf(w0) / (2.0f * tanf(pi * BREAKDOWN_BW_HZ / (float)ADC_RATE_HZ));

    emg.setNotch(hz, (float)ADC_RATE_HZ, q);
    settleNotch();
    float uv = windowAcMicrovolts(NULL, NULL);
    emg.disableNotch();
    return uv;
}

// AC RMS with a notch on every mains harmonic, settled first. Reports how many
// notches the library built through *sections, if given.
float measureWithMainsNotch(uint8_t *sections = NULL)
{
    uint8_t n = emg.setMainsNotch(MAINS_HZ, (float)ADC_RATE_HZ, NOTCH_BW_HZ);
    if (sections) *sections = n;
    settleNotch();
    float uv = windowAcMicrovolts(NULL, NULL);
    emg.disableNotch();
    return uv;
}

// Where is the interference actually sitting? Notch each mains harmonic in turn and
// see how much RMS each one removes, then all of them together. 50 Hz is the
// fundamental; a single 50 Hz notch leaves every harmonic above it untouched.
void harmonicBreakdown()
{
    Serial.println();
    Serial.println(F("--- HARMONIC BREAKDOWN ----------------------------------"));
    Serial.println(F("   Stay relaxed: this takes about 12 seconds."));
    nextSampleUs = micros();

    int16_t pk = 0;
    float   dc = 0.0f;
    float base = measureRaw(&pk, &dc);

    Serial.print(F("   unfiltered     : "));
    Serial.print(base, 1);
    Serial.print(F(" uV RMS   (peak "));
    Serial.print(pk);
    Serial.print(F(" cts, DC "));
    Serial.print(dc, 0);
    Serial.println(F(" cts)"));
    Serial.println();

    // Every harmonic below 0.45x the sample rate: 50-450 Hz, or 60-420 Hz.
    for (float hz = MAINS_HZ; hz <= 0.45f * (float)ADC_RATE_HZ; hz += MAINS_HZ) {
        float uv  = measureWithNotch(hz);
        float pct = (base > 0.01f) ? (100.0f * (base - uv) / base) : 0.0f;
        if (pct < 0.0f) pct = 0.0f;

        Serial.print(F("   notch "));
        if (hz < 100.0f) Serial.print(' ');
        Serial.print(hz, 0);
        Serial.print(F(" Hz   : "));
        Serial.print(uv, 1);
        Serial.print(F(" uV RMS   removes "));
        Serial.print(pct, 0);
        Serial.println(F("%"));
    }

    uint8_t sections = 0;
    float all    = measureWithMainsNotch(&sections);
    float allPct = (base > 0.01f) ? (100.0f * (base - all) / base) : 0.0f;
    if (allPct < 0.0f) allPct = 0.0f;
    Serial.print(F("   all harmonics  : "));
    Serial.print(all, 1);
    Serial.print(F(" uV RMS   removes "));
    Serial.print(allPct, 0);
    Serial.print(F("%   ("));
    Serial.print(sections);
    Serial.println(F(" notches)"));

    Serial.println();
    Serial.println(F("   If 50 Hz removes most of it, a single notch is enough."));
    Serial.println(F("   If 100 or 150 Hz removes the most, the hum is in the harmonics."));
    Serial.println(F("   Room lighting (LED drivers, fluorescent) and high electrode"));
    Serial.println(F("   impedance both do this, even on a battery-powered host. Re-run"));
    Serial.println(F("   'h' with the lights off: if the harmonics drop, it is the room."));
    Serial.println(F("   If none of them remove much, it is broadband, not mains."));
    Serial.println();
    Serial.println(F("   peak(cts)  DC(cts)  ac_raw(uV)  ac_notch(uV)  hum%   verdict"));
}

void phase5_signal()
{
    Serial.println(F("--- 5. Live signal --------------------------------------"));

    // Confirm the host can actually sustain the paced rate before relying on it.
    unsigned long t0 = millis();
    uint16_t n = 0;
    while (millis() - t0 < 1000) {
        emg.readRawUncorrected();
        n++;
    }
    Serial.print(F("   max achievable: "));
    Serial.print(n);
    Serial.print(F(" SPS    sampling paced at: "));
    Serial.print(ADC_RATE_HZ);
    Serial.println(F(" SPS"));
    if ((long)n < ADC_RATE_HZ) {
        Serial.println(F("   WARNING: host cannot sustain the paced rate; notch will be off-tune."));
    }
    nextSampleUs = micros();

    uint8_t sections = emg.setMainsNotch(MAINS_HZ, (float)ADC_RATE_HZ, NOTCH_BW_HZ);
    if (sections > 0) {
        Serial.print(F("   ac_notch: "));
        Serial.print(sections);
        Serial.print(F(" notches, "));
        Serial.print(NOTCH_BW_HZ, 0);
        Serial.print(F(" Hz wide, on "));
        Serial.print(MAINS_HZ, 0);
        Serial.print(F("-"));
        Serial.print(MAINS_HZ * sections, 0);
        Serial.println(F(" Hz"));
    } else {
        Serial.println(F("   SAMPLE RATE TOO LOW to build a notch."));
    }
    emg.disableNotch();

    Serial.println();
    Serial.println(F("   Full scale is +/-2048 counts. 1 count = 2.42 uV at the electrodes."));
    Serial.println(F("   Reference: relaxed muscle ~5 uV, light contraction ~100 uV,"));
    Serial.println(F("   strong contraction ~1000 uV RMS."));
    Serial.println();
    Serial.println(F("   >>> 's' + Enter = shorted-input test <<<"));
    Serial.println(F("   >>> 'h' + Enter = harmonic breakdown  <<<"));
    Serial.println();
    Serial.println(F("   peak(cts)  DC(cts)  ac_raw(uV)  ac_notch(uV)  hum%   verdict"));
}

// The decisive test. Bridge E1, E2 and REF together through a 10 kOhm resistor and
// plug that into the jack - no body in the loop at all. Whatever is left is the
// board and cable's own contribution.
void shortedInputTest()
{
    Serial.println();
    Serial.println(F("--- SHORTED-INPUT REFERENCE -----------------------------"));
    Serial.println(F("   Unplug the electrodes from your skin."));
    Serial.println(F("   Bridge E1 + E2 + REF together through a 10 kOhm resistor"));
    Serial.println(F("   and plug that into the 3.5 mm jack."));
    Serial.println(F("   Then type 'g' + Enter."));

    while (true) {
        if (Serial.available() && Serial.read() == 'g') break;
        delay(50);
    }

    Serial.println(F("   letting the front-end settle, then re-zeroing..."));
    // The chain has just come out of whatever state the electrodes had it in, and the
    // DC operating point moves when the source impedance changes. Settle before
    // baselining, or the baseline itself is the transient.
    for (uint16_t i = 0; i < 4000; i++) { emg.readRawUncorrected(); }
    emg.calibrateBaseline(512);

    int16_t peak = 0;
    float   dc   = 0.0f;
    float rawUv = measureRaw(&peak, &dc);

    float notchedUv = measureWithMainsNotch();

    Serial.print(F("   peak "));       Serial.print(peak);
    Serial.print(F(" counts   DC "));  Serial.print(dc, 0);
    Serial.print(F(" counts   ac ")); Serial.print(rawUv, 1);
    Serial.print(F(" uV   ac notched ")); Serial.print(notchedUv, 1);
    Serial.println(F(" uV"));
    Serial.println();

    if (rawUv < 25.0f) {
        Serial.println(F("   VERDICT: board + cable are CLEAN."));
        Serial.println(F("   Everything you saw on skin is real pickup at the"));
        Serial.println(F("   electrode/body interface. Work the electrodes, not the board:"));
        Serial.println(F("   REF firmly on bone, fresh gel pads, short leads."));
    } else if (rawUv < 200.0f) {
        Serial.println(F("   VERDICT: some board/cable pickup, but far below what"));
        Serial.println(F("   you measured on skin. The electrodes are still the"));
        Serial.println(F("   dominant term."));
    } else {
        Serial.println(F("   VERDICT: the BOARD OR CABLE is picking this up on its own,"));
        Serial.println(F("   with no body attached. Suspect the input network,"));
        Serial.println(F("   the cable, or the jack wiring."));
        Serial.println(F("   Re-run with the jack EMPTY to separate cable from board."));
    }

    Serial.println();
    Serial.println(F("   Reconnect the electrodes, then type 'r' + Enter to resume."));
    while (true) {
        if (Serial.available() && Serial.read() == 'r') break;
        delay(50);
    }
    emg.calibrateBaseline(512);
    nextSampleUs = micros();
    Serial.println();
    Serial.println(F("   peak(cts)  DC(cts)  ac_raw(uV)  ac_notch(uV)  hum%   verdict"));
}

void reportWindow()
{
    int16_t peak = 0;
    float   dc   = 0.0f;

    float rawUv     = measureRaw(&peak, &dc);
    float notchedUv = measureWithMainsNotch();

    float humPct = (rawUv > 0.01f) ? (100.0f * (rawUv - notchedUv) / rawUv) : 0.0f;
    if (humPct < 0.0f) humPct = 0.0f;

    Serial.print(F("   "));
    Serial.print(peak);
    Serial.print(F("\t     "));
    Serial.print(dc, 0);
    Serial.print(F("\t   "));
    Serial.print(rawUv, 1);
    Serial.print(F("\t     "));
    Serial.print(notchedUv, 1);
    Serial.print(F("\t  "));
    Serial.print(humPct, 0);
    Serial.print(F("%\t "));

    float dcAbs = (dc < 0.0f) ? -dc : dc;

    if (peak >= 2040) {
        Serial.println(F("RAILED - clipped at full scale"));
    } else if (dcAbs > 100.0f) {
        Serial.println(F("DC OFFSET - baseline is stale, re-zero (relax, then reset)"));
    } else if (peak > 1000) {
        Serial.println(F("HUGE - ~100x physiological. Not muscle."));
    } else if (humPct > 60.0f && rawUv > 100.0f) {
        Serial.println(F("hum dominated - electrodes or room lighting, not the board"));
    } else if (rawUv < 20.0f) {
        Serial.println(F("quiet - at or near the board noise floor"));
    } else if (notchedUv < 150.0f) {
        Serial.println(F("light contraction"));
    } else {
        Serial.println(F("strong contraction"));
    }
}

// ---- sketch ----------------------------------------------------------------------

void setup()
{
    Serial.begin(115200);
    unsigned long t0 = millis();
    while (!Serial && (millis() - t0 < 3000)) {
        delay(10);
    }

    Wire.begin();

    Serial.println();
    Serial.println(F("========== easyEMG diagnostics =========="));
    Serial.println();

    phase1_scan();
    if (!foundAddr) {
        Serial.println(F("Stopping - no ADC on the bus."));
        while (1) { delay(1000); }
    }

    phase2_config();
    phase3_pga();

    emg = easyEMG(foundAddr);
    emg.begin();
    Wire.setClock(400000);
    emg.calibrateBaseline();

    phase4_alive();
    phase5_signal();
}

void loop()
{
    if (Serial.available()) {
        char c = Serial.read();
        if (c == 's') shortedInputTest();
        if (c == 'h') harmonicBreakdown();
    }
    reportWindow();
}
