// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Ashwin Whitchurch, ProtoCentral Electronics
//
// 05.MuscleActivation_LED
//
// The classic muscle-sensor demo, and the starting point for prosthetics and
// gesture control: flex the muscle, the LED lights.
//
// The threshold is set automatically from YOUR resting noise floor rather than
// hard-coded, because the right number depends on electrode placement, skin prep
// and the person. The sketch measures the resting envelope, then triggers at a
// multiple of it.
//
// Hysteresis matters here. A single threshold makes the LED chatter whenever the
// envelope sits near it, so this uses separate ON and OFF levels - a standard
// Schmitt trigger, in software.
//
// For research and education only - not a medical device. Power the host from a battery
// or use an isolated Qwiic adapter when electrodes are on a person.

#include <Wire.h>
#include "ProtoCentral_easyEMG.h"

#ifndef LED_BUILTIN
#define LED_BUILTIN 2   // not defined for every ESP32 board; GPIO2 is the usual on-board LED
#endif

easyEMG emg;

const uint8_t LED_PIN = LED_BUILTIN;

// Trigger at 4x the resting noise floor, release at 2.5x.
const float TRIGGER_MULTIPLE = 4.0f;
const float RELEASE_MULTIPLE = 2.5f;

float onThreshold  = 0.0f;
float offThreshold = 0.0f;
bool  muscleOn     = false;

void setup()
{
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

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

    Serial.println("Relax the muscle. Measuring your resting noise floor...");
    delay(1000);
    emg.calibrateBaseline();
    emg.setEnvelopeWindow(32);

    // Average the resting envelope over 2 seconds rather than trusting one reading.
    float  sum   = 0.0f;
    uint32_t n   = 0;
    unsigned long t0 = millis();
    while (millis() - t0 < 2000) {
        sum += emg.readEnvelope();
        n++;
    }
    float restingLevel = (n > 0) ? (sum / (float)n) : 0.0f;

    onThreshold  = restingLevel * TRIGGER_MULTIPLE;
    offThreshold = restingLevel * RELEASE_MULTIPLE;
    emg.setActivationThreshold(onThreshold);

    Serial.print("Resting level : ");
    Serial.print(restingLevel, 2);
    Serial.println(" uV RMS");
    Serial.print("Trigger at    : ");
    Serial.print(onThreshold, 2);
    Serial.println(" uV RMS");
    Serial.print("Release at    : ");
    Serial.print(offThreshold, 2);
    Serial.println(" uV RMS");
    Serial.println();
    Serial.println("Flex the muscle.");
}

void loop()
{
    float envelope = emg.readEnvelope();

    // Schmitt trigger: different levels for turning on and off.
    if (!muscleOn && envelope >= onThreshold) {
        muscleOn = true;
        digitalWrite(LED_PIN, HIGH);
        Serial.print("FLEX  ");
        Serial.print(envelope, 1);
        Serial.println(" uV");
    } else if (muscleOn && envelope < offThreshold) {
        muscleOn = false;
        digitalWrite(LED_PIN, LOW);
        Serial.println("relax");
    }

    // emg.isActive() is the simple single-threshold version, if you do not want
    // hysteresis:
    //   if (emg.isActive()) { ... }
}
