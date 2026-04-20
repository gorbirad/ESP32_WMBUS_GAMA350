#include "wmbus_gama350.h"
#include <aes.h>
#include <string.h>

// Helper: convert hex string "32003316..." to bytes
static bool hexStringToBytes(const char* hex, uint8_t* bytes, size_t maxLen) {
    if (!hex || !bytes) return false;
    size_t len = strlen(hex);
    if (len % 2 != 0 || len / 2 > maxLen) return false;
    for (size_t i = 0; i < len; i += 2) {
        char buf[3] = {hex[i], hex[i+1], '\0'};
        bytes[i/2] = (uint8_t)strtol(buf, nullptr, 16);
    }
    return true;
}


Gama350Data decodeGama350(uint8_t* buf, int len, const char* keyHex) {
    Gama350Data out = {0, 0, false};

    if (buf == nullptr || len < 30 || keyHex == nullptr) return out;

    // GAMA350 transmits 2F 2F (encrypted) — skip unencrypted frames
    uint8_t lField = buf[0];
    if (buf[1] != 0x2F || buf[2] != 0x2F) return out;  // Not encrypted
    if (lField == 0 || lField > 127) return out;
    if ((int)lField + 1 > len) return out;

    // Convert hex key to bytes
    uint8_t keyBytes[16] = {0};
        if (!hexStringToBytes(keyHex, keyBytes, 16)) return out;

    // Extract IV from header: bytes 3-10 (A-field, M-field, ACC, CI)
    uint8_t iv[16] = {0};
    if (len >= 11) {
        memcpy(iv, &buf[3], 8);
    }

    // Payload starts at byte 11, encrypted portion
    int payload_start = 11;
    int payload_len = (int)lField + 1 - payload_start;
    if (payload_len < 16 || payload_len % 16 != 0) return out;

    // Decrypt AES-128-CBC
    uint8_t decrypted[256] = {0};
    memcpy(decrypted, &buf[payload_start], payload_len);

    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, keyBytes, iv);
    AES_CBC_decrypt_buffer(&ctx, decrypted, payload_len);

    // Parse decrypted OMS payload: DIF/VIF fields
    // GAMA350 format:
    // Byte 0: CI = 0x72 (variable length)
    // Bytes 1+: OBIS records (DIF/VIF/value)
    // OBIS 1.8.0 = energy (kWh)
    // OBIS 16.7.0 = power (W)

    int pos = 0;
    uint32_t obis_energy = 0;
    uint16_t obis_power = 0;
    bool found_energy = false, found_power = false;

    // Skip CI byte and any length prefix
    if (pos < payload_len && decrypted[pos] == 0x72) {
        pos++;
        if (pos < payload_len && decrypted[pos] < 0x10) pos++;  // Length byte if present
    }

    // Parse DIF/VIF records
    while (pos + 3 < payload_len && !(found_energy && found_power)) {
        uint8_t dif = decrypted[pos++];
        if (dif == 0x0F || dif == 0x00) break;  // End of data
        int dif_len = dif & 0x0F;  // Low nibble: length code

        uint8_t vif = decrypted[pos++];
        int vif_len = 1;
        if (vif & 0x80) {  // VIFE follows
            while (pos < payload_len && (decrypted[pos] & 0x80)) {
                pos++;
                vif_len++;
            }
            if (pos < payload_len) pos++;
            vif_len++;
        }

        // Determine field length based on DIF
        int field_len = 0;
        switch (dif_len) {
            case 0x01: field_len = 1; break;
            case 0x02: field_len = 2; break;
            case 0x03: field_len = 3; break;
            case 0x04: field_len = 4; break;
            case 0x05: field_len = 5; break;
            case 0x06: field_len = 6; break;
            case 0x07: field_len = 7; break;
            case 0x08: field_len = 8; break;
            default: field_len = 0;
        }

        // Check for OBIS codes by pattern matching
        // GAMA350 typically has 0x84 0x94 for 1.8.0 and 0x84 0xA4 for 16.7.0
        if ((dif == 0x84) && (vif == 0x94) && field_len == 4 && pos + 4 <= payload_len) {
            // OBIS 1.8.0: energy, 4 bytes, little-endian
            obis_energy = decrypted[pos] | (decrypted[pos+1] << 8) |
                          (decrypted[pos+2] << 16) | (decrypted[pos+3] << 24);
            found_energy = true;
            pos += field_len;
        } else if ((dif == 0x84) && (vif == 0xA4) && field_len == 2 && pos + 2 <= payload_len) {
            // OBIS 16.7.0: power, 2 bytes, little-endian
            obis_power = decrypted[pos] | (decrypted[pos+1] << 8);
            found_power = true;
            pos += field_len;
        } else {
            // Skip unknown field
            pos += field_len;
        }
    }

    // Validate extracted values
    if (obis_power > 50000) {
        found_power = false;  // Unrealistic power value
    }

    if (found_energy && found_power) {
        out.energy = obis_energy;
        out.power = obis_power;
        out.valid = true;
    }

    return out;
}