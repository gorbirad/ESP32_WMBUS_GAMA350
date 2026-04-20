#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>

extern "C" {
  #include "aes.h"
}

#include "wmbus_gama350.h"

// -------------------------
// KONFIGURACJA
// -------------------------

const char* ssid = "beskidlas";
const char* password = "RiU03082013Beskidzka";

const char* mqtt_server = "192.168.0.10";
const char* topic_power = "home/wmbus/gama350/power";
const char* topic_energy = "home/wmbus/gama350/energy";

uint32_t METER_ID = 0x31676464;

// Klucz AES licznika
uint8_t AES_KEY[16] = {
  0x32, 0x00, 0x33, 0x16,
  0x76, 0x46, 0x40, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// -------------------------
// MQTT
// -------------------------

WiFiClient espClient;
PubSubClient client(espClient);

// -------------------------

void decryptAES(uint8_t* data) {
  AES_ctx ctx;
  AES_init_ctx(&ctx, AES_KEY);
  AES_ECB_decrypt(&ctx, data);
}

// -------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(200);

  client.setServer(mqtt_server, 1883);

ELECHOUSE_cc1101.Init();

// częstotliwość
ELECHOUSE_cc1101.setMHZ(868.95);

// modulacja
ELECHOUSE_cc1101.setModulation(2);      // FSK

// bitrate
ELECHOUSE_cc1101.setDRate(32768);

// deviation
ELECHOUSE_cc1101.setDeviation(50);

// bandwidth
ELECHOUSE_cc1101.setRxBW(325);

// sync word
ELECHOUSE_cc1101.setSyncWord(0x54, 0x3D);

// Manchester
ELECHOUSE_cc1101.setManchester(1);

// fixed packet
ELECHOUSE_cc1101.setPktFormat(0);
ELECHOUSE_cc1101.setPacketLength(48);

// brak CRC
ELECHOUSE_cc1101.setCrc(0);

// brak whitening
ELECHOUSE_cc1101.setWhiteData(0);

// *** KLUCZOWE USTAWIENIA Z ESPHOME ***
ELECHOUSE_cc1101.SpiWriteReg(CC1101_MDMCFG2, 0x00);   // SYNC_MODE = 0 (no sync detect)
ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL2, 0x03);  // AGC
ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL1, 0x40);
ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL0, 0x91);
ELECHOUSE_cc1101.SpiWriteReg(CC1101_FIFOTHR, 0x47);   // FIFO threshold

ELECHOUSE_cc1101.SetRx();

ELECHOUSE_cc1101.SetRx();

Serial.println("CC1101 READY");
}

void loop() {
  if (!client.connected()) client.connect("ESP32-WMBUS");
  client.loop();

  // CheckRxFifo wymaga argumentu (timeout w ms)
  if (ELECHOUSE_cc1101.CheckRxFifo(1)) {

    uint8_t buf[64];
    byte len = ELECHOUSE_cc1101.ReceiveData(buf);

    Serial.print("RX len = ");
    Serial.println(len);

    for (int i = 0; i < len; i++) {
      if (buf[i] < 16) Serial.print("0");
      Serial.print(buf[i], HEX);
      Serial.print(" ");
    }
    Serial.println();

    uint32_t id = (buf[2] << 24) | (buf[3] << 16) | (buf[4] << 8) | buf[5];

    if (id != METER_ID) {
      Serial.print("Obca ramka: 0x");
      Serial.println(id, HEX);
      ELECHOUSE_cc1101.SetRx();
      return;
    }

    Serial.println("Ramka z mojego licznika!");

    decryptAES(buf + 12);

    auto data = decodeGama350(buf, len);

    if (data.valid) {
      client.publish(topic_energy, String(data.energy).c_str());
      client.publish(topic_power, String(data.power).c_str());

      Serial.print("ENERGY: ");
      Serial.println(data.energy);
      Serial.print("POWER: ");
      Serial.println(data.power);
    }

    ELECHOUSE_cc1101.SetRx();
  }
}