#pragma once
#include <Arduino.h>

struct Gama350Data {
    uint32_t energy;
    uint16_t power;
    bool valid;
};

struct AltMeterData {
    uint32_t energy;
    uint16_t power;
    bool valid;
    bool decrypted;
};

Gama350Data decodeGama350(uint8_t* buf, int len, const char* keyHex);
AltMeterData decodeAltMeterWithAES(uint8_t* buf, int len, const char* keyHex);