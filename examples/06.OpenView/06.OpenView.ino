// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Ashwin Whitchurch, ProtoCentral Electronics
//
// 06.OpenView
//
// Streams easyEMG data into the OpenView desktop app.
//
// ---- Which board to select in OpenView ---------------------------------------
//
//     In OpenView:  Connect -> USB -> select "easyEMG Breakout"
//
// This needs OpenView 3.4 or later. Earlier versions have no easyEMG profile and can't
// read this stream.
//
// ---- Packet format -----------------------------------------------------------
//
// ProtoCentral framing: 0x0A 0xFA <len> 0x00 <type> <payload...> 0x00 0x0B
// The payload is 8 bytes, little-endian:
//
//     [0-1]  sEMG          int16 (signed counts, +/-2048)
//     [2-3]  RMS envelope  int16 (counts, 0..2048), smoothed
//     [4-5]  %MVC          int16 (0 until an MVC has been recorded)
//     [6]    MVC step      0 no MVC yet, 1 relax, 2 contract now, 3 too weak, 4 MVC set
//     [7]    Reserved, always 0
//
// ---- Sample rate, and why this oversamples ------------------------------------
//
// Packets go out at 500 Hz. sEMG power sits mostly around 50-150 Hz, so a slower link
// shows little of it: a 125 Hz stream averaged down from 1000 SPS keeps only 23% of
// 100 Hz, and folds what's left down to 25 Hz.
//
// The ADC is read on a FIXED 1000 SPS schedule and each packet averages 2 samples.
// 1000 SPS puts Nyquist at 500 Hz, above the front end's 310 Hz corner, and the average
// keeps 95% of 100 Hz and 81% of 200 Hz. The little content between 250 and 310 Hz
// still folds down, reduced by the average and the analog roll-off. The envelope is fed
// from every 1 kHz sample, not the decimated one.
//
// The rate is FIXED rather than free-running for a specific reason: see the notch
// note below. A timed schedule also makes the sampling uniform, which every
// fixed-coefficient filter assumes.
//
// ---- Digital high-pass ---------------------------------------------------------
//
// HIGH_PASS_HZ adds a 2nd-order high-pass at 20 Hz. Without it, skin and electrode
// movement during a contraction can come through as slow swings many times the size of
// the EMG, and the envelope and %MVC count them as effort. It hides that artifact rather
// than curing it, so good electrode prep still matters: a big enough artifact clips the
// ADC. To see what it removes, set HIGH_PASS_HZ to 0 or run 07.Diagnostics.
//
// ---- Recording the MVC ---------------------------------------------------------
//
// %MVC is the envelope as a percentage of a maximum voluntary contraction (MVC). The
// step that records it runs after reset (RECORD_MVC), and again whenever the host
// sends 'm': OpenView's Record MVC button, or the Serial Monitor.
//
//   1. Relax for 2 seconds. The filters settle, and the last second is taken as your
//      resting level.
//   2. The on-board LED lights: contract as hard as you can until it goes out, 5
//      seconds later.
//
// The MVC is the highest smoothed envelope in those 5 seconds, the same envelope
// OpenView plots, and %MVC is worked out from that envelope too. The step refuses an
// MVC below MVC_MIN_UV or below MVC_MIN_REST_RATIO times your resting level: a
// reference taken from a relaxed or barely tensed muscle makes every later reading
// several hundred percent. A refused step blinks the LED and keeps the previous MVC.
//
// With MVC_AUTO_RAISE on, a later contraction stronger than the MVC becomes the new
// MVC, so %MVC never passes 100 and a weak reference corrects itself on the first
// hard contraction. A stray artifact that big raises it too, so turn it off for
// measurements and record the MVC properly.
//
// Opening the serial port resets some boards, which starts the step again: relax while
// you connect. Byte 6 of every packet carries the step's state, so OpenView can tell
// you when to relax and when to contract.
//
// ---- Mains notch ---------------------------------------------------------------
//
// USE_NOTCH arms the library's harmonic notch: a notch on every multiple of MAINS_HZ
// up to 450 Hz (50, 100 ... 450 Hz, or 60 ... 420 Hz), 6 Hz wide at the fundamental and
// 2 Hz wide on the harmonics.
//
// The hum on a person is rarely just 50 Hz. Even with the host on battery, LED drivers,
// fluorescent lights and nearby wiring put most of it at 100 and 150 Hz, which a single
// 50 Hz notch leaves untouched, and it can grow during a contraction. On one bench recording
// this took the resting level from about 80 to about 17 uV RMS. The notches together
// cost about 7% of the sEMG.
//
// The fundamental is wider because 50/60 Hz pickup is usually the largest and jumps
// when you move. A notch only removes a STEADY tone: after a jump it lets the change
// through until it settles, about 1/(pi x width). At 2 Hz that showed as 50 Hz bursts
// lasting about 0.4 s on a quiet trace; at 6 Hz they are over in about 0.1 s.
//
// It works here because the sample rate is FIXED and therefore exactly known. A 2 Hz
// notch at 450 Hz has to land within a fraction of a hertz, so its coefficients have
// to be built from the true sample rate. A free-running loop cannot hold its rate
// anywhere near tightly enough, and the per-packet serial write makes the intervals
// non-uniform on top of that. Pacing the ADC on a timer removes both problems.
//
// The mains frequency itself drifts by a fraction of a percent, and the drift grows
// with the harmonic, so the upper notches are shallower than the lower ones. Widen
// NOTCH_BW_HZ to trade more signal for more depth. On an 8-bit AVR the library stops
// at 150 Hz: it cannot run nine filters at 1000 SPS.
//
// A notch still costs real in-band signal, so turn it off for anything you intend to
// quote as a measurement.
//
// SAFETY: power the host from a BATTERY when the subject is a person. A
// mains-connected host couples the body to earth and the front-end saturates on
// common-mode hum - the symptom is no visible bursts at all, not merely a noisy
// trace. A battery host can still pick up hum from the room; that is what the notch
// is for.
//
// Download OpenView: https://github.com/Protocentral/protocentral_openview

#include <Wire.h>
#include "ProtoCentral_easyEMG.h"

#ifndef LED_BUILTIN
#define LED_BUILTIN 2   // not defined for every ESP32 board; GPIO2 is the usual on-board LED
#endif

easyEMG emg;

// ProtoCentral OpenView packet format constants
#define CES_CMDIF_PKT_START_1  0x0A
#define CES_CMDIF_PKT_START_2  0xFA
#define CES_CMDIF_TYPE_DATA    0x02
#define CES_CMDIF_PKT_STOP     0x0B
#define DATA_LEN               0x08
#define ZERO                   0

// Maximum voluntary contraction (MVC) for %MVC; see "Recording the MVC" above.
#define RECORD_MVC             true     // run the MVC step after reset
#define MVC_SETTLE_MS          2000L    // relax: filters settle, last second = rest level
#define MVC_RECORD_MS          5000L    // LED lit: contract as hard as you can
#define MVC_MIN_UV             100.0f   // refuse an MVC below this ...
#define MVC_MIN_REST_RATIO     5.0f     // ... or below this many times the rest level
#define MVC_AUTO_RAISE         true     // a stronger contraction becomes the new MVC
#define MVC_COMMAND            'm'      // host command that runs the step again

// Mains notch on every harmonic up to 450 Hz. Set MAINS_HZ to 50 or 60; set USE_NOTCH
// to false to disable. NOTCH_FUND_BW_HZ is the width at the fundamental, NOTCH_BW_HZ
// on each harmonic.
#define USE_NOTCH              true
#define MAINS_HZ               50.0f
#define NOTCH_FUND_BW_HZ       6.0f
#define NOTCH_BW_HZ            2.0f

// Digital high-pass corner; set to 0 to disable.
#define HIGH_PASS_HZ           20.0f

// Fixed ADC sampling rate. Must divide evenly into the packet rate. 1000 SPS gives
// 2 samples per packet and a 500 Hz Nyquist, clear of the 310 Hz front-end corner.
#define ADC_RATE_HZ            1000L

// Envelope smoothing time constant, milliseconds. The RMS window is limited to 128 samples,
// or 128 ms at 1000 SPS; this EMA makes the displayed envelope calmer on a chart.
#define ENVELOPE_SMOOTH_MS     120.0f

volatile uint8_t dataPacket[DATA_LEN];
const uint8_t dataPacketFooter[2] = {ZERO, CES_CMDIF_PKT_STOP};
const uint8_t dataPacketHeader[5] = {CES_CMDIF_PKT_START_1, CES_CMDIF_PKT_START_2,
                                  DATA_LEN, ZERO, CES_CMDIF_TYPE_DATA};

// 500 Hz packet rate, matching OpenView's easyEMG profile.
const unsigned long SAMPLE_INTERVAL_US = 2000;
const unsigned long ADC_INTERVAL_US    = 1000000L / ADC_RATE_HZ;
const uint16_t      SAMPLES_PER_PACKET = (uint16_t)(ADC_RATE_HZ / 500L);

unsigned long lastAdcTime = 0;

// Oversampling accumulator: every ADC sample taken between packets is averaged
// into the raw value that goes out.
long     rawAccum = 0;
uint16_t rawCount = 0;

// Exponentially smoothed envelope, and its per-packet coefficient.
float envSmooth = 0.0f;
const float ENV_ALPHA = (SAMPLE_INTERVAL_US / 1000.0f) / ENVELOPE_SMOOTH_MS;

// Microvolts at the electrodes per ADC count; set in setup() from the ADC range.
float uvPerCount = 0.0f;

// ---- MVC step ------------------------------------------------------------------
// Runs once per packet (500 Hz) on the smoothed envelope, in uV.

const uint16_t PACKETS_PER_SEC = 500;

enum MvcState : uint8_t {
    MVC_NONE     = 0,   // no MVC yet
    MVC_RELAX    = 1,   // settling; the last second is the rest level
    MVC_CONTRACT = 2,   // LED lit: contract now
    MVC_TOO_WEAK = 3,   // refused; shown for 2 s while the LED blinks
    MVC_SET      = 4,   // an MVC is in use
};

MvcState mvcState       = MVC_NONE;
uint16_t mvcPacketsLeft = 0;
float    mvcUv          = 0.0f;   // the MVC in use, 0 = none
float    mvcPeakUv      = 0.0f;   // highest envelope in the current step
float    restUv         = 0.0f;   // resting level from the last step
float    restSumUv      = 0.0f;
uint16_t restCount      = 0;

void startMvcStep()
{
    mvcState       = MVC_RELAX;
    mvcPacketsLeft = (uint16_t)((MVC_SETTLE_MS * PACKETS_PER_SEC) / 1000L);
    mvcPeakUv      = 0.0f;
    restSumUv      = 0.0f;
    restCount      = 0;
    digitalWrite(LED_BUILTIN, LOW);
}

// Smallest envelope accepted as an MVC.
float mvcThresholdUv()
{
    float t = MVC_MIN_REST_RATIO * restUv;
    return (t > MVC_MIN_UV) ? t : MVC_MIN_UV;
}

void updateMvc(float envUv)
{
    switch (mvcState) {
    case MVC_RELAX:
        if (mvcPacketsLeft <= PACKETS_PER_SEC) {     // last second of the settle
            restSumUv += envUv;
            restCount++;
        }
        if (--mvcPacketsLeft == 0) {
            restUv         = (restCount > 0) ? (restSumUv / (float)restCount) : 0.0f;
            mvcState       = MVC_CONTRACT;
            mvcPacketsLeft = (uint16_t)((MVC_RECORD_MS * PACKETS_PER_SEC) / 1000L);
            digitalWrite(LED_BUILTIN, HIGH);
        }
        return;

    case MVC_CONTRACT:
        if (envUv > mvcPeakUv) {
            mvcPeakUv = envUv;
        }
        if (--mvcPacketsLeft == 0) {
            digitalWrite(LED_BUILTIN, LOW);
            if (mvcPeakUv >= mvcThresholdUv()) {
                mvcUv    = mvcPeakUv;
                mvcState = MVC_SET;
                emg.setMVC(mvcUv);
            } else {
                mvcState       = MVC_TOO_WEAK;
                mvcPacketsLeft = 2 * PACKETS_PER_SEC;
            }
        }
        return;

    case MVC_TOO_WEAK:
        digitalWrite(LED_BUILTIN, ((mvcPacketsLeft / 50) & 1) ? HIGH : LOW);   // 5 Hz blink
        if (--mvcPacketsLeft == 0) {
            digitalWrite(LED_BUILTIN, LOW);
            mvcState = (mvcUv > 0.0f) ? MVC_SET : MVC_NONE;
        }
        break;

    default:
        break;
    }

#if MVC_AUTO_RAISE
    if (envUv >= mvcThresholdUv() && envUv > mvcUv) {
        mvcUv = envUv;
        emg.setMVC(mvcUv);
        if (mvcState == MVC_NONE) {
            mvcState = MVC_SET;
        }
    }
#endif
}

void sendDataPacket(int16_t emgCounts, int16_t envelope, int16_t percentMvc,
                    uint8_t mvcStep)
{
    dataPacket[0] = emgCounts;
    dataPacket[1] = emgCounts >> 8;

    dataPacket[2] = envelope;
    dataPacket[3] = envelope >> 8;

    dataPacket[4] = percentMvc;
    dataPacket[5] = percentMvc >> 8;

    dataPacket[6] = mvcStep;
    dataPacket[7] = 0;

    for (int i = 0; i < 5; i++) {
        Serial.write(dataPacketHeader[i]);
    }
    for (int i = 0; i < DATA_LEN; i++) {
        Serial.write(dataPacket[i]);
    }
    for (int i = 0; i < 2; i++) {
        Serial.write(dataPacketFooter[i]);
    }
}

void setup()
{
    // OpenView's easyEMG profile connects at 115200 baud.
    Serial.begin(115200);

    unsigned long startTime = millis();
    while (!Serial && (millis() - startTime < 3000)) {
        delay(10);
    }

    if (!emg.begin()) {
        // No text output - the host is expecting binary packets.
        while (1) {
            delay(500);
        }
    }

    Wire.setClock(400000);

    // Zero the resting offset. Stay relaxed through this, and through any reset -
    // calibrating while flexed biases every later reading low.
    emg.calibrateBaseline();

    // Longest window the library holds: 128 samples, which at the fixed 1000 SPS is
    // 128 ms of RMS - squarely inside the usual 50-200 ms range for sEMG. A window's
    // sample count means nothing without the rate beside it.
    emg.setEnvelopeWindow(easyEMG::EASYEMG_MAX_ENVELOPE_WINDOW);

#if USE_NOTCH
    // The rate is fixed and known exactly, so the coefficients are exact. Builds one
    // notch per harmonic: 9 at 50 Hz mains, 7 at 60 Hz, 3 on AVR.
    emg.setMainsNotch(MAINS_HZ, (float)ADC_RATE_HZ, NOTCH_BW_HZ, NOTCH_FUND_BW_HZ);
#endif

    // Leaves the filter off when HIGH_PASS_HZ is 0.
    emg.setHighPass(HIGH_PASS_HZ, (float)ADC_RATE_HZ);

    uvPerCount = emg.fullScaleVolts() / 2048.0f * 1000000.0f / EASYEMG_GAIN;

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
#if RECORD_MVC
    startMvcStep();
#endif

    envSmooth   = 0.0f;
    rawAccum    = 0;
    rawCount    = 0;
    lastAdcTime = micros();
}

void loop()
{
    unsigned long now = micros();
    if ((now - lastAdcTime) < ADC_INTERVAL_US) {
        return;
    }

    // Advance by whole intervals so the sampling rate stays exactly ADC_RATE_HZ,
    // which is what the notch coefficients were built from. Resync only if we have
    // fallen badly behind, which would mean the host cannot sustain the rate.
    if ((now - lastAdcTime) > (4 * ADC_INTERVAL_US)) {
        lastAdcTime = now;
    } else {
        lastAdcTime += ADC_INTERVAL_US;
    }

    // One timed sample: feeds the envelope at full bandwidth and accumulates into
    // the average that becomes the decimated raw value.
    int16_t envelope = emg.readEnvelopeRaw();
    rawAccum += emg.lastRaw();
    rawCount++;

    if (rawCount < SAMPLES_PER_PACKET) {
        return;
    }

    int16_t raw = (int16_t)(rawAccum / (long)rawCount);
    rawAccum = 0;
    rawCount = 0;

    // Smooth the envelope for display. The RMS window is bounded by the library's
    // buffer, so this is what actually sets how calm the trace looks.
    envSmooth += ((float)envelope - envSmooth) * ENV_ALPHA;

    // 'm' from the host (OpenView's Record MVC button) runs the MVC step again.
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c == MVC_COMMAND || c == (MVC_COMMAND - 'a' + 'A')) {
            startMvcStep();
        }
    }

    float envUv = envSmooth * uvPerCount;
    updateMvc(envUv);

    int16_t pctMVC = (mvcUv > 0.0f) ? (int16_t)(envUv / mvcUv * 100.0f + 0.5f) : 0;

    sendDataPacket(raw, (int16_t)(envSmooth + 0.5f), pctMVC, (uint8_t)mvcState);
}
