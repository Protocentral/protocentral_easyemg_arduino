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
// The payload is 8 bytes, all int16 little-endian:
//
//     [0-1]  sEMG          (signed counts, +/-2048)
//     [2-3]  RMS envelope  (counts, 0..2048)
//     [4-5]  %MVC          (0 until an MVC has been recorded)
//     [6-7]  Reserved, always 0
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
// With RECORD_MVC on, the on-board LED lights about a second after reset: contract as
// hard as you can until it goes out, 5 seconds later. Data streams throughout, so you
// can watch it in OpenView. Opening the serial port resets some boards, which starts
// the step again.
//
// %MVC stays 0 until the step finishes, and stays 0 afterwards if the peak was below
// MVC_MIN_UV, because a reference taken from a relaxed muscle gives meaningless
// readings. Press reset to try again.
//
// ---- Mains notch ---------------------------------------------------------------
//
// USE_NOTCH arms the library's 50/60 Hz notch, and it works here because the sample
// rate is FIXED and therefore exactly known.
//
// That matters more than it sounds. A Q=4 notch at 50 Hz is only about 12.5 Hz wide,
// so its coefficients have to be built from the true sample rate. Get the rate wrong
// by 10% and the notch lands at 45 Hz, where it attenuates mains by under 4 dB -
// effectively not working, while still removing signal at the wrong frequency. A
// free-running loop cannot hold its rate anywhere near tightly enough, and the
// per-packet serial write makes the intervals non-uniform on top of that. Pacing the
// ADC on a timer removes both problems.
//
// A notch still costs real in-band signal, so turn it off for anything you intend to
// quote as a measurement.
//
// SAFETY: power the host from a BATTERY when the subject is a person. A
// mains-connected host couples the body to earth and the front-end saturates on
// common-mode hum - the symptom is no visible bursts at all, not merely a noisy
// trace.
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

// Record a maximum voluntary contraction after reset so OpenView can show %MVC.
// Set to false to skip the step; %MVC then stays 0.
#define RECORD_MVC             true
#define MVC_SETTLE_MS          1000L    // lets the filters settle before recording
#define MVC_RECORD_MS          5000L
#define MVC_MIN_UV             100.0f   // about a light contraction

// Mains notch. Set MAINS_HZ to 50 or 60; set USE_NOTCH to false to disable.
#define USE_NOTCH              true
#define MAINS_HZ               50.0f

// Digital high-pass corner; set to 0 to disable.
#define HIGH_PASS_HZ           20.0f

// Fixed ADC sampling rate. Must divide evenly into the packet rate. 1000 SPS gives
// 2 samples per packet and a 500 Hz Nyquist, clear of the 310 Hz front-end corner.
#define ADC_RATE_HZ            1000L

// Envelope smoothing time constant, milliseconds. The RMS window is limited to 128 samples,
// or 128 ms at 1000 SPS; this EMA makes the displayed envelope calmer on a chart.
#define ENVELOPE_SMOOTH_MS     120.0f

volatile char dataPacket[DATA_LEN];
const char dataPacketFooter[2] = {ZERO, CES_CMDIF_PKT_STOP};
const char dataPacketHeader[5] = {CES_CMDIF_PKT_START_1, CES_CMDIF_PKT_START_2,
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

#if RECORD_MVC
// Samples left in the MVC step, counting down through settling and then recording.
long  mvcSamplesLeft = 0;
float mvcPeak        = 0.0f;

// Called once per ADC sample, so the step shares the paced rate the notch and high-pass
// were built for.
void updateMvcStep()
{
    if (mvcSamplesLeft == 0) {
        return;
    }
    mvcSamplesLeft--;

    const long recordSamples = (MVC_RECORD_MS * ADC_RATE_HZ) / 1000L;
    if (mvcSamplesLeft >= recordSamples) {
        if (mvcSamplesLeft == recordSamples) {
            digitalWrite(LED_BUILTIN, HIGH);
        }
        return;
    }

    float e = emg.lastEnvelope();
    if (e > mvcPeak) {
        mvcPeak = e;
    }

    if (mvcSamplesLeft == 0) {
        digitalWrite(LED_BUILTIN, LOW);
        if (mvcPeak >= MVC_MIN_UV) {
            emg.setMVC(mvcPeak);
        }
    }
}
#endif

void sendDataPacket(int16_t emgCounts, int16_t envelope, int16_t percentMvc)
{
    dataPacket[0] = emgCounts;
    dataPacket[1] = emgCounts >> 8;

    dataPacket[2] = envelope;
    dataPacket[3] = envelope >> 8;

    dataPacket[4] = percentMvc;
    dataPacket[5] = percentMvc >> 8;

    dataPacket[6] = 0;
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
    // The rate is fixed and known exactly, so the coefficients are exact.
    emg.setNotch(MAINS_HZ, (float)ADC_RATE_HZ);
#endif

    // Leaves the filter off when HIGH_PASS_HZ is 0.
    emg.setHighPass(HIGH_PASS_HZ, (float)ADC_RATE_HZ);

#if RECORD_MVC
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    mvcSamplesLeft = ((MVC_SETTLE_MS + MVC_RECORD_MS) * ADC_RATE_HZ) / 1000L;
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

#if RECORD_MVC
    updateMvcStep();
#endif

    if (rawCount < SAMPLES_PER_PACKET) {
        return;
    }

    int16_t raw = (int16_t)(rawAccum / (long)rawCount);
    rawAccum = 0;
    rawCount = 0;

    // Smooth the envelope for display. The RMS window is bounded by the library's
    // buffer, so this is what actually sets how calm the trace looks.
    envSmooth += ((float)envelope - envSmooth) * ENV_ALPHA;

    int16_t pctMVC = (int16_t)emg.readPercentMVC();

    sendDataPacket(raw, (int16_t)(envSmooth + 0.5f), pctMVC);
}
