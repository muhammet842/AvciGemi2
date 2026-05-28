#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "DHT.h"

// --- SENSÖR PİNLERİ VE ADRESLERİ ---
#define TdsSensorPin A0
#define Mq9AnalogPin A1   
#define Mq9DigitalPin 4   
#define ONE_WIRE_BUS 2
#define DHTPIN 3
#define DHTTYPE DHT11
#define VREF 5.0
#define SCOUNT 10
#define MAG_ADDR 0x1E

// --- POMPA / RÖLE PİNİ ---
#define POMPA_PIN 5

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
DHT dht(DHTPIN, DHTTYPE);

// --- ZAMANLAMA VE POMPA DEĞİŞKENLERİ ---
unsigned long lastSendTime = 0;
const unsigned long sendInterval = 1500;

bool pompaCalisiyor = false;
unsigned long pompaBaslangicZamani = 0;
const unsigned long pompaCalismaSuresi = 19000;

void setup() {
  Serial.begin(115200);   
  Serial1.begin(115200);
  
  sensors.begin(); 
  dht.begin();
  pinMode(TdsSensorPin, INPUT);
  pinMode(Mq9AnalogPin, INPUT);
  pinMode(Mq9DigitalPin, INPUT);

  digitalWrite(POMPA_PIN, HIGH); 
  pinMode(POMPA_PIN, OUTPUT);

  Wire.begin();
  Wire.beginTransmission(MAG_ADDR);
  Wire.write(0x02); 
  Wire.write(0x00); 
  Wire.endTransmission();

  Serial.println("Sistem Hazir: Veri gonderim araligi 1.5sn olarak ayarlandi.");
}

void loop() {
  // Pompa komutu alma
  if (Serial1.available()) {
    String gelenKomut = Serial1.readStringUntil('\n');
    gelenKomut.trim();
    if (gelenKomut == "PUMP_ON" && !pompaCalisiyor) {
      pompaCalisiyor = true;
      pompaBaslangicZamani = millis();
      digitalWrite(POMPA_PIN, LOW);
      Serial.println(">> Pompa BAŞLATILDI");
    }
  }

  // Pompa süresi kontrolü
  if (pompaCalisiyor && (millis() - pompaBaslangicZamani >= pompaCalismaSuresi)) {
    digitalWrite(POMPA_PIN, HIGH);
    pompaCalisiyor = false;
    Serial.println(">> Pompa DURDURULDU");
  }

  // Sensör okuma ve gönderme
  if (millis() - lastSendTime >= sendInterval) {
    lastSendTime = millis();

    sensors.requestTemperatures();
    float waterTemp = sensors.getTempCByIndex(0);
    if (waterTemp == -127.0 || waterTemp == 85.0) waterTemp = 25.0;

    float airTemp = dht.readTemperature();
    float airHum  = dht.readHumidity();
    if (isnan(airTemp)) airTemp = 0.0;
    if (isnan(airHum))  airHum  = 0.0;

    int mq9Val   = analogRead(Mq9AnalogPin);
    int mq9Alarm = digitalRead(Mq9DigitalPin);

    // Manyetik sensör - DOĞRU SIRA: X, Y, Z
    Wire.beginTransmission(MAG_ADDR);
    Wire.write(0x03);
    Wire.endTransmission(false);
    Wire.requestFrom(MAG_ADDR, 6);
    int16_t mx = (Wire.read() << 8) | Wire.read();
    int16_t my = (Wire.read() << 8) | Wire.read();
    int16_t mz = (Wire.read() << 8) | Wire.read();

    float heading = atan2(my, mx) * 180.0 / PI;
    if (heading < 0) heading += 360.0;

    // TDS ölçümü
    long sum = 0;
    for (int i = 0; i < 5; i++) {
      sum += analogRead(TdsSensorPin);
      delayMicroseconds(1000);
    }
    float averageVoltage = (sum / 5.0) * VREF / 1024.0;
    float compCoeff = 1.0 + 0.02 * (waterTemp - 25.0);
    float compVolts = averageVoltage / compCoeff;
    float tdsValue = (133.42 * pow(compVolts, 3) - 255.86 * pow(compVolts, 2) + 857.39 * compVolts) * 0.5;

    Serial1.print("TDS:");   Serial1.print(tdsValue, 1);
    Serial1.print(",WTMP:"); Serial1.print(waterTemp, 1);
    Serial1.print(",HEAD:"); Serial1.print(heading, 1);
    Serial1.print(",ATMP:"); Serial1.print(airTemp, 1);
    Serial1.print(",HUM:");  Serial1.print(airHum, 1);
    Serial1.print(",GAZ:");  Serial1.print(mq9Val);
    Serial1.print(",ALARM:");Serial1.print(mq9Alarm == LOW ? 1 : 0);
    Serial1.print(",MX:");   Serial1.print(mx);
    Serial1.print(",MY:");   Serial1.print(my);
    Serial1.print(",MZ:");   Serial1.println(mz);

    Serial.println(">> Paket gönderildi. GAZ=" + String(mq9Val));
  }
}