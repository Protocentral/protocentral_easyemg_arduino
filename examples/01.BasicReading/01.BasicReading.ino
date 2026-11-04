// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Ashwin Whitchurch, ProtoCentral Electronics
//
// 01.BasicReading
//
// The "hello world": initialise the board and print the raw muscle signal in ADC
// counts and in microvolts at the electrodes.
//
// Hardware connections (Qwiic / STEMMA QT or breakout header):
//   VCC -> 3.3V        SDA -> I2C Data
//   GND -> Ground      SCL -> I2C Clock
//
// Electrode placement (biceps or forearm flexor):
//   E1 and E2 on the muscle belly, about 20 mm apart, along the muscle fibres.
//   REF on a bony landmark away from muscle - the elbow or the wrist.
//   Wipe the skin with alcohol and let it dry, or you will chase artifacts that
//   are not the board's fault.
//
// SAFETY: easyEMG has no patient isolation and is not a medical device. When the
// subject is a person, power your host from a battery - never from mains USB.

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

    Serial.println("ProtoCentral easyEMG ready. Relax the muscle...");
    delay(500);

    // Zero the resting offset. Keep the muscle relaxed for this.
    emg.calibrateBaseline();
    Serial.print("Baseline: ");
    Serial.print(emg.getBaseline());
    Serial.println(" counts");

    Serial.println("Raw(counts)\tSignal(uV)");
}

void loop()
{
    int16_t raw        = emg.readRaw();
    float   microvolts = emg.readMicrovolts();

    Serial.print(raw);
    Serial.print("\t");
    Serial.println(microvolts, 1);

    delay(10);   // ~100 Hz print rate
}
