// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Ashwin Whitchurch, ProtoCentral Electronics
//
// 03.Calibration
//
// Zeroes the resting offset and measures your noise floor. Do this every session, per
// subject: the board reads differentially against its own buffered mid-rail, so the
// resting count is already near zero, but amplifier offset leaves a few counts behind.
// calibrateBaseline() averages the resting signal and subtracts it.
//
// The front-end gain (x51.7) is set by the onboard resistors, so there is nothing to
// calibrate there. The sketch prints it for reference.
//
// For research and education only - not a medical device. Power the host from a battery
// or use an isolated Qwiic adapter when electrodes are on a person.

#include <Wire.h>
#include "ProtoCentral_easyEMG.h"

easyEMG emg;

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

    Serial.println("=== easyEMG calibration ===");
    Serial.print("Front-end gain        : x");
    Serial.println(EASYEMG_GAIN, 2);
    Serial.print("ADC full scale        : +/-");
    Serial.print(emg.fullScaleVolts(), 3);
    Serial.println(" V");
    Serial.print("Resolution at the skin: ");
    Serial.print((emg.fullScaleVolts() / 2048.0f) * 1000000.0f / EASYEMG_GAIN, 2);
    Serial.println(" uV per count");
    Serial.println();

    Serial.println("Relax the muscle completely. Measuring baseline in 3 seconds...");
    delay(3000);

    emg.calibrateBaseline(512);

    Serial.print("Baseline offset: ");
    Serial.print(emg.getBaseline());
    Serial.println(" counts");
    Serial.println();

    // Noise floor: with the muscle relaxed, the envelope should sit close to the
    // board's quantisation limit. If it is much higher, suspect mains pickup, long
    // electrode leads, or poor skin prep.
    Serial.println("Measuring resting noise floor (2 s)...");
    emg.setEnvelopeWindow(64);
    unsigned long t0 = millis();
    while (millis() - t0 < 2000) {
        emg.readEnvelope();
    }
    Serial.print("Resting envelope: ");
    Serial.print(emg.lastEnvelope(), 2);
    Serial.println(" uV RMS");
    Serial.println();

    Serial.println("Copy this line into your own sketch:");
    Serial.print("  emg.setBaseline(");
    Serial.print(emg.getBaseline());
    Serial.println(");");
}

void loop()
{
    delay(1000);
}
