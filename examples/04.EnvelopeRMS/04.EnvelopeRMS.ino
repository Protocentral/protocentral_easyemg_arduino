// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Ashwin Whitchurch, ProtoCentral Electronics
//
// 04.EnvelopeRMS
//
// Surface EMG is a zero-mean AC signal, so averaging it gives you roughly nothing.
// The number that actually means "how hard is this muscle working" is the RMS over
// a sliding window - the envelope.
//
// This sketch prints the envelope in microvolts and, once you have recorded a
// maximum voluntary contraction (MVC), as a percentage of it. %MVC is how sports
// science and rehab compare muscle effort across subjects and sessions, because
// raw microvolts depend on electrode placement, skin, and body composition.
//
// Window sizing: the window is in SAMPLES, not milliseconds, because the achievable
// sample rate depends on your host. Use envelopeWindowMillis() with your measured
// loop rate to see what your window works out to in time. 100 ms is the usual
// starting point for sEMG.
//
// For research and education only - not a medical device. Power the host from a battery
// or use an isolated Qwiic adapter when electrodes are on a person.

#include <Wire.h>
#include "ProtoCentral_easyEMG.h"

easyEMG emg;

unsigned long lastPrint   = 0;
unsigned long sampleCount = 0;
unsigned long rateStart   = 0;
float         sampleRate  = 0.0f;

void setup()
{
    Serial.begin(115200);

    unsigned long startTime = millis();
    while (!Serial && (millis() - startTime < 3000)) {
        delay(10);
    }

    if (!emg.begin()) {
        Serial.println("easyEMG not found - check wiring and I2C address (0x48).");
        while (1) {
            delay(1000);
        }
    }

    Serial.println("Relax the muscle. Zeroing...");
    delay(1000);
    emg.calibrateBaseline();

    emg.setEnvelopeWindow(64);

    // Record a maximum voluntary contraction so readings can be normalised.
    Serial.println();
    Serial.println("Contract the muscle AS HARD AS YOU CAN for the next 5 seconds.");
    delay(2000);
    Serial.println("GO!");

    float peak = 0.0f;
    unsigned long t0 = millis();
    while (millis() - t0 < 5000) {
        float e = emg.readEnvelope();
        if (e > peak) {
            peak = e;
        }
    }

    emg.setMVC(peak);
    Serial.print("MVC recorded: ");
    Serial.print(peak, 1);
    Serial.println(" uV RMS");
    Serial.println("Relax. Streaming...");
    Serial.println();
    Serial.println("Envelope(uV)\t%MVC\tRate(SPS)");

    rateStart = millis();
}

void loop()
{
    emg.readEnvelope();
    sampleCount++;

    // Print at 20 Hz - fast enough to feel live, slow enough to read.
    if (millis() - lastPrint >= 50) {
        lastPrint = millis();

        unsigned long elapsed = millis() - rateStart;
        if (elapsed >= 1000) {
            sampleRate  = (sampleCount * 1000.0f) / (float)elapsed;
            sampleCount = 0;
            rateStart   = millis();
        }

        Serial.print(emg.lastEnvelope(), 1);
        Serial.print("\t");
        Serial.print(emg.readPercentMVC(), 1);
        Serial.print("\t");
        Serial.println(sampleRate, 0);
    }
}
