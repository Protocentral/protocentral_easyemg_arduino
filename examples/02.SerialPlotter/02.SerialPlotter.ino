// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Ashwin Whitchurch, ProtoCentral Electronics
//
// 02.SerialPlotter
//
// Plots muscle activity in the Arduino IDE's Serial Plotter
// (Tools -> Serial Plotter, 115200 baud).
//
// ---- Why this sketch paces its printing --------------------------------------
//
// The Serial Plotter keeps a fixed-size window of points, so the time span you see
// on screen is (window size / print rate). Printing every sample - which is what
// you want for a true raw waveform - scrolls a whole contraction past in a
// fraction of a second.
//
// So this sketch SAMPLES as fast as it can, which the RMS envelope needs to be
// valid, but PRINTS at a set rate. Set PLOT_RATE_HZ below to trade detail for
// time on screen:
//
//   PLOT_RATE_HZ = 0    print every sample. True raw waveform, scrolls very fast.
//                       Use this to look at the shape of the signal itself.
//   PLOT_RATE_HZ = 50   default. Roughly 10 s on screen with a 500-point window,
//                       or 1 s with a 50-point one. Good for watching contractions.
//   PLOT_RATE_HZ = 20   slower still - a long view for fatigue or repetition work.
//
// When pacing is on, the first trace is PEAK-HOLD: the largest |sample| seen since
// the last print, not a decimated sample. Plain decimation would randomly miss the
// peaks of a burst and under-report how hard you contracted; peak-hold does not.
//
// For research and education only - not a medical device. Power the host from a battery
// or use an isolated Qwiic adapter when electrodes are on a person.

#include <Wire.h>
#include "ProtoCentral_easyEMG.h"

easyEMG emg;

const uint16_t PLOT_RATE_HZ = 50;      // 0 = print every sample

unsigned long printIntervalUs = 0;
unsigned long lastPrint       = 0;
int16_t       peakHold        = 0;

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

    // Faster I2C where the host supports it - more samples into every envelope.
    Wire.setClock(400000);

    Serial.println("Relax the muscle. Zeroing...");
    delay(1000);
    emg.calibrateBaseline();
    emg.setEnvelopeWindow(32);

    printIntervalUs = (PLOT_RATE_HZ > 0) ? (1000000UL / PLOT_RATE_HZ) : 0;

    // Plotter legend. Two traces either way, but they mean different things
    // depending on whether pacing is on.
    if (PLOT_RATE_HZ > 0) {
        Serial.println("Peak:,Envelope:");
    } else {
        Serial.println("Raw:,Envelope:");
    }

    lastPrint = micros();
}

void loop()
{
    // One conversion per iteration: this samples AND updates the envelope.
    int16_t envelope = emg.readEnvelopeRaw();
    int16_t raw      = emg.lastRaw();

    if (printIntervalUs == 0) {
        // Unpaced: every sample goes out as a true raw waveform.
        Serial.print(raw);
        Serial.print(",");
        Serial.println(envelope);
        return;
    }

    // Paced: hold the largest magnitude seen since the last print.
    int16_t magnitude = (raw < 0) ? (int16_t)(-raw) : raw;
    if (magnitude > peakHold) {
        peakHold = magnitude;
    }

    unsigned long now = micros();
    if (now - lastPrint >= printIntervalUs) {
        lastPrint = now;

        Serial.print(peakHold);
        Serial.print(",");
        Serial.println(envelope);

        peakHold = 0;
    }
}
