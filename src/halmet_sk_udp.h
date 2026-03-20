#pragma once
// Signal K UDP broadcaster for HALMET
//
// Broadcasts Signal K delta messages via UDP multicast (239.2.5.26:4445),
// the address defined in the Signal K specification.  Any SK client on the
// same subnet (OpenCPN, Kip, Signal K Instrumentpanel, etc.) will receive
// the data automatically — no server IP configuration required.
//
// Usage in main.cpp:
//   #include "halmet_sk_udp.h"
//   SetupSKUDP();                                    // call once after get_app()
//   PublishSKFloat("tanks.fuel.main.currentLevel", 0.75f);  // call from consumers
//   PublishSKBool ("propulsion.main.alternatorVoltage", ...);

#include <Arduino.h>

namespace halmet {

// Call once in setup(), after sensesp_app = builder.get_app().
void SetupSKUDP();

// Queue a float value for the next broadcast delta.
// path  – Signal K path, e.g. "tanks.fuel.main.currentLevel"
// value – numeric value in SI units (fraction 0-1 for levels, Hz for revs, etc.)
void PublishSKFloat(const String& path, float value);

// Queue a boolean value (e.g. alarm states).
void PublishSKBool(const String& path, bool value);

}  // namespace halmet
