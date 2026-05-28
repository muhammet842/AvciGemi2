#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <SoftwareSerial.h>
#include <math.h>

extern "C" {
  #include "user_interface.h"
}

#define PI 3.14159265358979323846

// ---------------- WiFi Ayarları ----------------
const char* ssid = "AvciGemi";
const char* password = "AvcI_1324";
ESP8266WebServer server(80);

// --------------- Raspberry PI Tarafı -----------
WiFiUDP udp;
const char* piIP = "192.168.0.127";
const int piPort = 5005;

// ---------------- MOTOR GÜVENLİK WATCHDOG ----------------
unsigned long lastMotorCommandTime = 0;
const unsigned long MOTOR_TIMEOUT_MS = 2500;   
bool motorWatchdogEnabled = true;

// ---------------- AKTİF GEOFENCE KORUMA DEĞİŞKENLERİ ----------------
bool geofenceViolation = false;
unsigned long lastGeofenceCorrection = 0;
const unsigned long GEOFENCE_CORRECTION_INTERVAL = 1000;
const unsigned long STOP_TIME = 800;      
const unsigned long BACKWARD_TIME = 500;

// ----------------  GEOFENCE DEĞİŞKENLERİ ----------------
bool geofenceEnabled = true;
float geofenceLat = 41.0082;
float geofenceLon = 28.9784;
float geofenceRadius = 30.0;
const float DANGER_ZONE_METERS = 3.0;

// ---------------- SIM808 Ayarları ----------------
SoftwareSerial gpsSerial(D6, D7); // RX, TX

// ---------------- Sistem Değişkenleri ----------------
unsigned long lastGpsReq = 0;
unsigned long lastGsmReq = 0;
unsigned long lastGpsResponse = 0;
unsigned long lastGsmResponse = 0;
unsigned long lastGasRead = 0; 

bool gpsConnected = false;
bool gsmConnected = false;

// ---------------- Sensör & Durum Verileri ----------------
float waterTemp = 0.0, waterTDS = 0.0;
float airTemp = 0.0, airHum = 0.0;
String waterQuality = "Bekleniyor...";
String gsmSignal = "0", batteryLevel = "75", currentMode = "MANUEL";

bool pompaAktif = false;
String gasStatus = "Isinma...";
int gasValue = 0;
int gasAlarm = 0;

int magX = 0, magY = 0, magZ = 0;
float totalMag = 0.0;

String currentLatitude = "0.0", currentLongitude = "0.0", currentSpeed = "0.0";
String currentHeading = "0.0", satellites = "0", hdopValue = "0.0";
String gpsStatusStr = "Uydu Bekleniyor";

// ---------------- Donanım Pinleri ----------------
const int IN1 = D2;
const int IN2 = D3;
const int IN3 = D4;
const int IN4 = D8;

extern const char index_html[];

// ---------------- Veri Ayıklama Fonksiyonu ----------------
String getValue(String data, char sep, int index) {
  int found = 0, start = 0, end = -1;
  for (int i = 0; i < (int)data.length(); i++) {
    if (data[i] == sep || i == (int)data.length() - 1) {
      found++;
      start = end + 1;
      end = (i == (int)data.length() - 1) ? (int)data.length() : i;
      if (found - 1 == index) return data.substring(start, end);
    }
  }
  return "";
}

// ---------------- MOTOR KONTROLLERİ - SON VE EN SAĞLAM VERSİYON ----------------
void motorStop() {
    analogWrite(IN1, 0);
    analogWrite(IN2, 0);
    analogWrite(IN3, 0);
    analogWrite(IN4, 0);
}

void motorForward() {
    analogWrite(IN1, 255);   // Sol İLERİ
    analogWrite(IN2, 0);
    analogWrite(IN3, 255);   // Sağ İLERİ
    analogWrite(IN4, 0);
}

void motorBackward() {
    analogWrite(IN1, 0);
    analogWrite(IN2, 255);   // Sol GERİ
    analogWrite(IN3, 0);
    analogWrite(IN4, 255);   // Sağ GERİ
}

void motorLeft() {
    analogWrite(IN1, 255);   // Sol tam ileri
    analogWrite(IN2, 0);
    analogWrite(IN3, 0);     // Sağ dur (sert dönüş için)
    analogWrite(IN4, 0);
}

void motorRight() {
    analogWrite(IN1, 0);
    analogWrite(IN2, 0);
    analogWrite(IN3, 255);   // Sağ tam ileri
    analogWrite(IN4, 0);
}

// ---------------- API Endpoint'leri ----------------
void handleKeyControl() {
  String key = server.arg("value");
  key.toUpperCase();

  // Her başarılı komutta watchdog timer'ını sıfırla
  lastMotorCommandTime = millis();

  if      (key == "W") motorForward();
  else if (key == "S") motorBackward();
  else if (key == "A") motorLeft();
  else if (key == "D") motorRight();
  else                 motorStop();

  server.send(200, "text/plain", "OK");
}

float calculateDistance(float lat1, float lon1, float lat2, float lon2) {
    float r = 6371000.0; // Dünya'nın yarıçapı (metre)
    
    // Dereceleri radyana çeviriyoruz
    float lat1_rad = lat1 * PI / 180.0;
    float lon1_rad = lon1 * PI / 180.0;
    float lat2_rad = lat2 * PI / 180.0;
    float lon2_rad = lon2 * PI / 180.0;

    float dLat = lat2_rad - lat1_rad;
    float dLon = lon2_rad - lon1_rad;

    float a = sin(dLat / 2.0) * sin(dLat / 2.0) +
              cos(lat1_rad) * cos(lat2_rad) *
              sin(dLon / 2.0) * sin(dLon / 2.0);
              
    float c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return r * c;
}

// ---------------- SMS Gönderme Endpoint’i ----------------
void handleSendSMS() {
  String phone = server.arg("phone");
  if (phone.length() < 8) {
    server.send(400, "application/json", "{\"status\":\"PHONE_MISSING\"}");
    return;
  }

  if (!gpsConnected) {
    server.send(200, "application/json", "{\"status\":\"GPS_NOT_READY\"}");
    return;
  }

  sendLocationSMS(phone);
  server.send(200, "application/json", "{\"status\":\"QUEUED\"}");
}

// ---------------- Dinamik Geofence Ayar Endpoint’i ----------------
void handleSetGeofence() {
  String enabledStr = server.arg("enabled");
  String radiusStr  = server.arg("radius");
  String latStr     = server.arg("lat");
  String lonStr     = server.arg("lon");

  if (enabledStr.length() > 0) {
    geofenceEnabled = (enabledStr == "1");
  }
  if (radiusStr.length() > 0) {
    geofenceRadius = radiusStr.toFloat();
    if (geofenceRadius < 5.0) geofenceRadius = 5.0;   // minimum güvenlik
  }
  if (latStr.length() > 0 && lonStr.length() > 0) {
    geofenceLat = latStr.toFloat();
    geofenceLon = lonStr.toFloat();
  }

  Serial.printf("GEOFENCE GÜNCELLENDİ → Enabled:%d | Radius:%.1f m | Center: %.6f,%.6f\n",
                geofenceEnabled, geofenceRadius, geofenceLat, geofenceLon);

  server.send(200, "application/json", "{\"status\":\"OK\",\"enabled\":" + String(geofenceEnabled) + 
              ",\"radius\":" + String(geofenceRadius, 1) + "}");
}

// ---------------- Sistem Modu Değiştirme Endpoint’i ----------------
void handleSetMode() {
  String newMode = server.arg("value");
  if (newMode.length() > 0) {
    currentMode = newMode;
    Serial.println("Sistem modu değiştirildi: " + currentMode);
  }
  server.send(200, "application/json", "{\"status\":\"OK\",\"mode\":\"" + currentMode + "\"}");
}

// Geofence ihlal kontrolü + aktif koruma + Watchdog
void checkGeofence() {
    
}

void handleGpsData() {
  char json[256];
  snprintf(json, sizeof(json), 
    "{\"latitude\":\"%s\",\"longitude\":\"%s\",\"speed\":\"%s\",\"air_temp\":%.1f,\"air_hum\":%.1f,\"heading\":\"%s\",\"sats\":\"%s\",\"hdop\":\"%s\",\"status\":\"%s\",\"datetime\":\"31/03/2026 16:23\"}",
    currentLatitude.c_str(), currentLongitude.c_str(), currentSpeed.c_str(), airTemp, airHum, currentHeading.c_str(), satellites.c_str(), hdopValue.c_str(), gpsStatusStr.c_str()
  );
  server.send(200, "application/json", json);
}

void sendLocationSMS(String phoneNumber) {
  if (gpsConnected) {
    gpsSerial.println("AT+CMGF=1"); 
    delay(50); // Minimum bekleme
    
    gpsSerial.print("AT+CMGS=\"");
    gpsSerial.print(phoneNumber);
    gpsSerial.println("\"");
    delay(50);

    gpsSerial.print("Konum: https://www.google.com/maps?q=");
    gpsSerial.print(currentLatitude);
    gpsSerial.print(",");
    gpsSerial.print(currentLongitude);
    gpsSerial.print("\nEnlem: ");
    gpsSerial.print(currentLatitude);
    gpsSerial.print("\nBoylam: ");
    gpsSerial.print(currentLongitude);
    gpsSerial.print("\nUydu: ");
    gpsSerial.print(satellites);
    
    delay(50);
    gpsSerial.write(26);
    
    Serial.println("SMS Gonderildi: " + phoneNumber);
  } else {
    Serial.println("Hata: GPS bagli degil, SMS iptal edildi.");
  }
}

void handleWaterData() {
  char json[128];
  snprintf(json, sizeof(json), "{\"tds\":%.1f,\"water_temp\":%.1f,\"quality\":\"%s\"}", waterTDS, waterTemp, waterQuality.c_str());
  server.send(200, "application/json", json);
}

void updateModemData() {
  static String modemBuffer = "";
  
  if (millis() - lastGpsReq > 2000) {
    gpsSerial.println("AT+CGNSINF");
    lastGpsReq = millis();
  }
  if (millis() - lastGsmReq > 15000) {
    gpsSerial.println("AT+CSQ");
    lastGsmReq = millis();
  }

  while (gpsSerial.available()) {
    char c = gpsSerial.read();
    if (c == '\n' || c == '\r') {
      modemBuffer.trim(); // Boşlukları temizle
      
      if (modemBuffer.startsWith("+CGNSINF:")) {
        lastGpsResponse = millis();
        gpsConnected = true;

        String fixStatus = getValue(modemBuffer, ',', 1); 
        String latRaw    = getValue(modemBuffer, ',', 3);
        String lonRaw    = getValue(modemBuffer, ',', 4);

        // --- GÜVENLİK FİLTRESİ ---
        // Koordinat sadece rakam ve nokta içermeli, 'A', 'T' gibi harfler içermemeli
        bool isLonValid = true;
        for(int i=0; i<lonRaw.length(); i++) {
          if(!isdigit(lonRaw[i]) && lonRaw[i] != '.') {
            isLonValid = false; 
            break;
          }
        }

        if (fixStatus == "1" && isLonValid && latRaw.length() > 5) {
          currentLatitude  = latRaw;
          currentLongitude = lonRaw;
          currentSpeed     = getValue(modemBuffer, ',', 6);
          hdopValue        = getValue(modemBuffer, ',', 10);
          satellites       = getValue(modemBuffer, ',', 14);
          gpsStatusStr = "GPS Aktif";
        } else {
          gpsStatusStr = "Uydu Bekleniyor (Fix Yok)...";
        }
      }
      
      else if (modemBuffer.startsWith("+CSQ:")) {
        lastGsmResponse = millis();
        gsmConnected = true;
        String csqStr = getValue(modemBuffer, ':', 1);
        csqStr.trim();
        csqStr = getValue(csqStr, ',', 0);
        int rssi = csqStr.toInt();
        gsmSignal = (rssi == 99 || rssi == 0) ? "Yok" : String(-113 + (rssi * 2)) + " dBm";
      }
      
      modemBuffer = "";
    } 
    else if (c >= 32) {
      modemBuffer += c;
    }
  }

  if (millis() - lastGpsResponse > 12000) {
    gpsConnected = false;
    gpsStatusStr = "Modül Bağlantısı Koptu";
  }
}

void handleAllData() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send(200, "application/json", "");

  server.sendContent("{");
  server.sendContent("\"lat\":\"" + currentLatitude + "\",");
  server.sendContent("\"lon\":\"" + currentLongitude + "\",");
  server.sendContent("\"speed\":\"" + currentSpeed + "\",");
  server.sendContent("\"head\":\"" + currentHeading + "\",");
  server.sendContent("\"sats\":\"" + satellites + "\",");
  server.sendContent("\"gps_status\":\"" + gpsStatusStr + "\",");
  server.sendContent("\"mag_x\":" + String(magX) + ",");
  server.sendContent("\"mag_y\":" + String(magY) + ",");
  server.sendContent("\"mag_z\":" + String(magZ) + ",");
  server.sendContent("\"mag_total\":" + String(totalMag, 1) + ",");
  server.sendContent("\"tds\":" + String(waterTDS, 1) + ",");
  server.sendContent("\"water_temp\":" + String(waterTemp, 1) + ",");
  server.sendContent("\"air_temp\":" + String(airTemp, 1) + ",");
  server.sendContent("\"air_hum\":" + String(airHum, 1) + ",");
  server.sendContent("\"quality\":\"" + waterQuality + "\",");
  server.sendContent("\"gas_val\":" + String(gasValue) + ",");
  server.sendContent("\"gas_alarm\":" + String(gasAlarm) + ",");
  server.sendContent("\"gas_status\":\"" + gasStatus + "\",");
  server.sendContent("\"wifi_rssi\":" + String(WiFi.RSSI()) + ",");
  server.sendContent("\"gsm_signal\":\"" + gsmSignal + "\","); 
  server.sendContent("\"batt\":\"" + batteryLevel + "\",");
  server.sendContent("\"mode\":\"" + currentMode + "\",");
  
  // Geofence durumu JS ile senkronize ediliyor
  server.sendContent("\"geofence_enabled\":" + String(geofenceEnabled) + ",");
  server.sendContent("\"geofence_radius\":" + String(geofenceRadius, 1) + ",");
  server.sendContent("\"geofence_lat\":" + String(geofenceLat, 6) + ",");
  server.sendContent("\"geofence_lon\":" + String(geofenceLon, 6));

  server.sendContent("}");
  server.sendContent(""); // Gönderimi bitir
}

void readMegaData() {
  static String megaBuffer = "";
  
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      megaBuffer.trim();
      
      if (megaBuffer.length() > 40) {
        // Yeni ve daha güvenli parsing yöntemi
        int start = 0;
        int commaIndex;
        
        while ((commaIndex = megaBuffer.indexOf(',', start)) != -1 || start < megaBuffer.length()) {
          String part;
          if (commaIndex == -1) {
            part = megaBuffer.substring(start);
            start = megaBuffer.length();
          } else {
            part = megaBuffer.substring(start, commaIndex);
            start = commaIndex + 1;
          }
          
          part.trim();
          
          if (part.startsWith("TDS:")) {
            waterTDS = part.substring(4).toFloat();
          }
          else if (part.startsWith("WTMP:")) {
            waterTemp = part.substring(5).toFloat();
          }
          else if (part.startsWith("HEAD:")) {
            currentHeading = part.substring(5);
            currentHeading.trim();
          }
          else if (part.startsWith("ATMP:")) {
            airTemp = part.substring(5).toFloat();
          }
          else if (part.startsWith("HUM:")) {
            airHum = part.substring(4).toFloat();
          }
          else if (part.startsWith("GAZ:")) {
            gasValue = part.substring(4).toInt();
          }
          else if (part.startsWith("ALARM:")) {
            gasAlarm = part.substring(6).toInt();
          }
          else if (part.startsWith("MX:")) {
            magX = part.substring(3).toInt();
          }
          else if (part.startsWith("MY:")) {
            magY = part.substring(3).toInt();
          }
          else if (part.startsWith("MZ:")) {
            magZ = part.substring(3).toInt();
          }
        }

        gasStatus = (gasAlarm == 1) ? "TEHLIKE" : 
                    (gasValue > 600) ? "TEHLIKE" : 
                    (gasValue > 300) ? "Dikkat" : "Normal";

        totalMag = sqrt(pow(magX, 2) + pow(magY, 2) + pow(magZ, 2));

        if (waterTDS < 100)      waterQuality = "Temiz";
        else if (waterTDS < 300) waterQuality = "Orta";
        else                     waterQuality = "Kirli";

        Serial.println("Mega verisi işlendi → Gaz: " + String(gasValue) + " | Alarm: " + String(gasAlarm));
      }
      
      megaBuffer = "";
    } 
    else if (c >= 32) {
      megaBuffer += c;
    }
  }
}

void setup() {

  // 1. BEKLEME VE VOLTAJ OTURTMA (BURAYA EKLE)
  delay(3000); // Powerbank'in uyanması ve voltajın oturması için 3 saniye süre tanı.

  // İşlemciyi 160MHz moduna zorla.
  system_update_cpu_freq(160);
  
  Serial.begin(115200); 
  delay(500);
  
  gpsSerial.begin(9600);
  
  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  motorStop();

  lastMotorCommandTime = millis();   // Başlangıçta timer'ı sıfırla

  server.on("/api/mode", handleSetMode);

  server.on("/sendsms", handleSendSMS);

  server.on("/setgeofence", handleSetGeofence);
  
  server.on("/pompala", HTTP_GET, []() {
    Serial.println("PUMP_ON"); 
    server.send(200, "application/json", "{\"status\":\"OK\"}");
  });
  
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  delay(500); 
  WiFi.begin(ssid, password);
  
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 40) {
    delay(500); 
    attempt++;
    if(attempt % 5 == 0) Serial.print(".");
  }

  if(WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Bağlandı!");
    Serial.print("IP Adresi: ");
    Serial.println(WiFi.localIP());
    udp.begin(4211);
  } else {
    Serial.println("\nWiFi Bağlantısı BAŞARISIZ! Cihaz resetleniyor...");
    ESP.restart(); // Bağlanamazsa zombi modunda kalmasın, kendini baştan başlatsın.
  }

  server.on("/", []() { server.send_P(200, "text/html", index_html); });
  
  server.on("/systemstatus", []() {
    char json[200];
    snprintf(json, sizeof(json), "{\"wifi\":\"Aktif\",\"gsm_signal\":\"%s\",\"battery\":\"%s\",\"current_mode\":\"%s\",\"mag_total\":%.1f}", 
             gsmSignal.c_str(), batteryLevel.c_str(), currentMode.c_str(), totalMag);
    server.send(200, "application/json", json);
  });
  
  server.on("/savewater", []() {
    String payload = "{";
    payload += "\"type\":\"mission_data\",";
    payload += "\"TDS\":" + String(waterTDS, 1) + ",";
    payload += "\"Sicaklik\":" + String(waterTemp, 1) + ",";
    payload += "\"Enlem\":\"" + (currentLatitude.length() < 2 ? "0.0" : currentLatitude) + "\",";
    payload += "\"Boylam\":\"" + (currentLongitude.length() < 2 ? "0.0" : currentLongitude) + "\",";
    payload += "\"Gaz\":" + String(gasValue) + ",";
    payload += "\"Alarm\":" + String(gasAlarm) + ",";
    payload += "\"MagX\":" + String(magX) + ",";
    payload += "\"MagY\":" + String(magY) + ",";
    payload += "\"MagZ\":" + String(magZ) + ",";
    payload += "\"MagTotal\":" + String(totalMag, 1);
    payload += "}";

    int udpStatus = udp.beginPacket(piIP, piPort);
    if (udpStatus) {
        udp.write(payload.c_str());
        udp.endPacket();
        server.send(200, "application/json", "{\"status\":\"SUCCESS\"}");
    } else {
        server.send(500, "application/json", "{\"status\":\"UDP_ERROR\"}");
    }
  });

  server.on("/gpsdata", handleGpsData);
  server.on("/key", handleKeyControl);
  server.on("/waterdata", handleWaterData);
  server.on("/api/all", handleAllData);

  server.on("/gas", []() {
    char json[100];
    snprintf(json, sizeof(json), "{\"value\":%d,\"status\":\"%s\",\"alarm\":%d}", gasValue, gasStatus.c_str(), gasAlarm);
    server.send(200, "application/json", json);
  });

  server.begin();

  Serial.printf("Başlangıç Free Heap: %d bytes\n", ESP.getFreeHeap());

  gpsSerial.println("ATE0");
  delay(100);

  gpsSerial.println("AT+CGNSPWR=1"); 
}

void loop() {
  server.handleClient(); 
  yield();

  // Her 800ms'de bir çalışsın
  static unsigned long lastModemUpdate = 0;
  if (millis() - lastModemUpdate >= 800) {
    updateModemData();
    lastModemUpdate = millis();
  }

  // Her 400ms'de bir çalışsın
  static unsigned long lastMegaUpdate = 0;
  if (millis() - lastMegaUpdate >= 400) {
    readMegaData();
    lastMegaUpdate = millis();
  }

  checkGeofence();

  static unsigned long lastHeapPrint = 0;
  if (millis() - lastHeapPrint > 5000) {
    Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
    lastHeapPrint = millis();
  }

  yield();
}

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang='tr'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1'>
    <title>Gemi Takip & Kontrol</title>
    <link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css" />
    <link href="https://fonts.googleapis.com/css2?family=Roboto:wght@400;700;900&display=swap" rel="stylesheet">

    <style>
    /* Haritanın Tam Görünmesi İçin Zorunlu CSS */
    html, body {
        height: 100%;
        margin: 0;
        padding: 0;
    }

    body {
        font-family: 'Roboto', sans-serif;
        display: flex;
        flex-direction: column;
        height: 100vh;
        background-color: #ecf0f1;
    }

    /* HEADER BAR STİLİ */
    #header-bar {
        display: flex;
        justify-content: space-between;
        align-items: center;
        padding: 5px 20px;
        background-color: #2c3e50;
        color: white;
        font-size: 0.9em;
        font-weight: bold;
        box-shadow: 0 2px 4px rgba(0,0,0,0.2);
        z-index: 1000;
    }

    .status-group {
        display: flex;
        gap: 20px;
        align-items: center;
    }

    .status-item {
        display: flex;
        align-items: center;
        gap: 5px;
    }

    /* MOD SEÇİMİ STİLİ */
    #mode-selector {
        padding: 3px 8px;
        border-radius: 4px;
        border: 1px solid #3498db;
        background-color: #34495e;
        color: white;
        cursor: pointer;
        font-weight: bold;
        font-size: 0.9em;
    }

    #mode-selector option {
        background-color: #34495e;
        color: white;
    }

    /* ANA İÇERİK (HARİTA + PANEL) */
    #main-content {
        flex: 1;
        height: calc(100vh - 50px);
        overflow: hidden;
        display: flex;
    }

    #map-container {
        flex: 3.8;
        display: flex;
        height: 100%;
        overflow: hidden;
        flex-direction: column;
    }

    #map {
        flex: 1;
        height: 100% !important;
    }

    #serial-log {
        height: 200px;
        background-color: #111;
        color: #00ff88;
        font-family: monospace;
        font-size: 13px;
        padding: 8px;
        overflow-y: auto;
        border-top: 2px solid #333;
    }

    #panel {
        flex: 6.2;
        background-color: #f4f4f4;
        display: flex;
        flex-direction: column;
        align-items: center;
        padding: 20px;
        box-shadow: -2px 0 5px rgba(0,0,0,0.1);
        overflow-y: auto;
        max-height: 100%;
    }

    /* KAMERA VE BUTONLAR İÇİN YATAY KAPSAYICI */
    .top-controls-container {
        width: 95%;
        display: flex;
        justify-content: space-between;
        gap: 15px;
        margin-bottom: 20px;
        padding: 10px;
        background-color: #fefefe;
        border-radius: 8px;
        box-shadow: 0 1px 3px rgba(0,0,0,0.1);
    }

    /* KAMERA KUTUSU */
    .camera-box-wrapper {
        flex: 2.2;
        min-width: 220px;
        display: flex;
        flex-direction: column;
        gap: 5px;
    }

    .camera-box {
        aspect-ratio: 16 / 9;
        background-color: #34495e;
        border-radius: 8px;
        display: flex;
        align-items: center;
        justify-content: center;
        color: #ecf0f1;
        font-weight: bold;
        border: 2px dashed #95a5a6;
        text-align: center;
        position: relative;
        overflow: hidden;
    }

    /* SOL ALT: HIZ VE YÖN BİLGİSİ KUTUSU */
    #speed-heading-display {
        position: absolute;
        bottom: 5px;
        left: 5px;
        background-color: rgba(0, 0, 0, 0.6);
        color: white;
        padding: 5px 8px;
        border-radius: 4px;
        font-size: 0.8em;
        font-weight: normal;
        line-height: 1.2;
        text-align: left;
    }

    /* SAĞ ÜST: KAMERA DURUM BİLGİSİ KUTUSU */
    #camera-status-display {
        position: absolute;
        top: 5px;
        right: 5px;
        background-color: rgba(0, 0, 0, 0.6);
        color: #2ecc71;
        padding: 5px 8px;
        border-radius: 4px;
        font-size: 0.8em;
        font-weight: bold;
        line-height: 1.2;
        text-align: right;
        border: 1px solid #2ecc71;
    }

    /* POWER BAR STİLİ */
    .power-info {
        background-color: white;
        padding: 8px 10px;
        border-radius: 4px;
        box-shadow: 0 1px 3px rgba(0,0,0,0.1);
    }

    .progress-container {
        width: 100%;
        background-color: #ecf0f1;
        border-radius: 6px;
        overflow: hidden;
        margin-bottom: 5px;
    }

    .progress-bar {
        height: 18px;
        width: 75%;
        background-color: #2ecc71;
        color: white;
        text-align: center;
        line-height: 18px;
        font-size: 0.8em;
        transition: width 0.5s;
    }

    #power-duration {
        font-size: 0.9em;
        color: #7f8c8d;
        font-weight: bold;
    }

    .info-box {
        background: white;
        padding: 15px;
        border-radius: 8px;
        width: 95%;
        margin-bottom: 20px;
        box-shadow: 0 2px 5px rgba(0,0,0,0.05);
        text-align: center;
    }

    #coords {
        font-size: 1.1em;
        color: #e67e22;
        font-weight: bold;
    }

    #datetime-display {
        font-size: 1.1em;
        color: #2980b9;
        font-weight: bold;
    }

    /* BUTONLAR SÜTUNU */
    .button-column {
        flex: 0 0 155px;
        display: flex;
        flex-direction: column;
        gap: 10px;
    }

    button {
        font-size: 14px;
        padding: 8px;
        cursor: pointer;
        border: none;
        border-radius: 5px;
        transition: 0.3s;
        width: 100%;
        font-weight: bold;
        text-transform: uppercase;
        letter-spacing: 1px;
    }

    .btn-motor {
        background-color: #3498db;
        color: white;
        box-shadow: 0 3px 0 #2980b9;
    }
    .btn-motor:active {
        background-color: #2980b9;
        transform: translateY(1px);
        box-shadow: 0 2px 0 #2980b9;
    }

    .btn-sms {
        background-color: #3498db;
        color: white;
        box-shadow: 0 3px 0 #2980b9;
    }
    .btn-sms:active {
        background-color: #2980b9;
        transform: translateY(1px);
        box-shadow: 0 2px 0 #2980b9;
    }

    .btn-secondary {
        background-color: #95a5a6;
        color: white;
        box-shadow: 0 3px 0 #7f8c8d;
    }
    .btn-secondary:active {
        background-color: #7f8c8d;
        transform: translateY(1px);
        box-shadow: 0 2px 0 #7f8c8d;
    }

    .btn-warning {
        background-color: #f39c12;
        color: white;
        box-shadow: 0 3px 0 #e67e22;
    }
    .btn-warning:active {
        background-color: #e67e22;
        transform: translateY(1px);
        box-shadow: 0 2px 0 #e67e22;
    }

    /* GEOFENCE İÇİN INPUT VE LABEL STİLİ */
    .geofence-input-container {
        width: 100%;
        margin-bottom: 5px;
        display: flex;
        flex-direction: column;
        align-items: flex-start;
    }
    .geofence-input-container label {
        font-size: 0.8em;
        color: #2c3e50;
        margin-bottom: 3px;
        font-weight: bold;
    }
    .geofence-input-container input {
        padding: 6px;
        width: 100%;
        box-sizing: border-box;
        border: 1px solid #ccc;
        border-radius: 4px;
        text-align: center;
        font-weight: bold;
    }

    /* Rota Noktası İşaretçisi Stili */
    .waypoint-marker-container div {
        font-family: 'Roboto', sans-serif;
        font-size: 16px !important;
        color: white !important;
        font-weight: 900 !important;
        background-color: #d81b60 !important;
        border-radius: 50% !important;
        width: 28px !important;
        height: 28px !important;
        line-height: 28px !important;
        text-align: center !important;
        border: 2px solid #a3003c !important;
        box-shadow: 0 2px 4px rgba(0,0,0,0.4);
    }

    /* GEOFENCE STİLİ */
    .geofence-circle {
        fill-color: #f39c12;
        fill-opacity: 0.2;
        color: #d35400;
        weight: 2;
        opacity: 0.7;
    }

    /* TABLET İÇİN EK İYİLEŞTİRME */
    @media (max-width: 1024px) {
        #map-container { flex: 3.5; }
        #panel { flex: 6.5; }
    }

    @media (max-width: 768px) {
        body {
            flex-direction: column;
        }

        #main-content {
            flex-direction: column;
        }

        #header-bar {
            flex-direction: column;
            gap: 5px;
            padding: 10px;
        }

        .status-group {
            flex-direction: column;
            gap: 5px;
        }

        #map {
            flex: 1;
            height: 45vh;
            width: 100%;
            border-right: none;
            border-bottom: 2px solid #bdc3c7;
        }

        #panel {
            flex: 1;
            height: auto;
            width: 100%;
            padding: 15px 10px;
        }

        .top-controls-container {
            flex-direction: column;
            gap: 12px;
            width: 95%;
        }

        .camera-box-wrapper {
            min-width: 100%;
            flex: none;
        }

        .button-column {
            flex: 0 0 auto;
            width: 100%;
        }

        .camera-box {
            aspect-ratio: 16 / 9;
        }
    }
</style>

<div id="header-bar">
    <div class="status-group">
        <div class="status-item">
            <span style="font-size: 1.2em;">🌐</span>
            Bağlantı: <span id="wifi-status">Yükleniyor...</span>
        </div>
        <div class="status-item">
            <span style="font-size: 1.2em;">📡</span>
            GSM: <span id="gsm-info">Yükleniyor...</span>
        </div>
        <div class="status-item">
            <span style="font-size: 1.2em;">🛰</span>
            GPS: <span id="gps-fix-info">Yükleniyor...</span>
        </div>
        <div class="status-item">
            <span style="font-size: 1.2em;">🔋</span>
            Batarya: <span id="battery-info">Yükleniyor...</span>
        </div>
    </div>
</div>

<div id="main-content">
    <div id="map-container">
        <div id="map"></div>
        <div id="serial-log"></div>
    </div>
    <div id="panel">
        <div class="top-controls-container">
            <div class="camera-box-wrapper">
                <div class="camera-box">
                    <iframe 
                        src="/live/stream.html?src=c500&mode=mse&autoplay=1"
                        loading="lazy"
                        style="width:100%; height:100%; border:none;"
                        allow="autoplay">
                    </iframe>

                    <div id="speed-heading-display">
                        Hız: Yükleniyor...<br>
                        Yön: Yükleniyor...<br>
                    </div>

                    <div id="camera-status-display">
                        Gecikme: <span id="camera-latency">Ultra Low</span><br>
                        Bağlantı: <span id="camera-connection">WebRTC</span>
                    </div>
                </div>

                <div class="power-info">
                    <div style="font-size: 0.9em; font-weight: bold; margin-bottom: 3px;">Kalan Güç: <span id="power-percentage">75%</span></div>
                    <div class="progress-container">
                        <div class="progress-bar" id="power-progress">75%</div>
                    </div>
                    <div>Tahmini Çalışma Süresi: <span id="power-duration">~ 6 Saat 30 Dakika</span></div>
                </div>
            </div>

            <div class="button-column">
                <button id="motor-btn" class="btn-motor" onclick="toggleMotor()" style="background-color: #e74c3c; box-shadow: 0 3px 0 #c0392b;">MOTORLARI ÇALIŞTIR</button>
                <button class="btn-sms" onclick="sendLocationSms()">KONUM SMS'İ GÖNDER</button>
                <button class="btn-secondary" id="follow-toggle" onclick="toggleFollow()">KONUM TAKİBİ: AÇIK</button>

                <div class="geofence-input-container">
                    <label for="geofence-radius">Geofence Yarıçapı (metre)</label>
                    <input type="number" id="geofence-radius" value="100" min="10" placeholder="Yarıçap (metre)">
                </div>
                <button class="btn-warning" onclick="geofenceControl()">GEOFENCE ETKİNLEŞTİR / DEVRE DIŞI</button>
                <button class="btn-motor" id="water-toggle-btn" onclick="showWaterPanel()">SU NUMUNESİ AL</button>
            </div>
        </div>

        <div class="info-box" style="display: flex; justify-content: space-around;">
            <div>
                <div>Konum Koordinatları:</div>
                <div id="coords">Yükleniyor...</div>
            </div>
            <div>
                <div>Yerel Saat:</div>
                <div id="datetime-display">Yükleniyor...</div>
            </div>
        </div>

        <div class="info-box" id="route-summary-info" style="display:none; background-color: #e3f2fd; border: 1px solid #1565c0;">
            <div style="font-size: 1.1em; color: #1565c0; font-weight: bold; margin-bottom: 5px;">ÇOKLU ROTA ÖZETİ (Tıklayarak Rota Çizin)</div>
            <div style="text-align: left; padding: 5px;">
                <div style="font-weight: bold;">Toplam Uzaklık: <span id="target-distance" style="float: right; color: #1565c0;">0 metre</span></div>
                <div>Tahmini Varış (ETA): <span id="target-eta" style="float: right; color: #00897b;">Hesaplanıyor...</span></div>
                <div>Gerekli Yakıt: <span id="required-fuel" style="float: right; color: #d81b60;">Hesaplanıyor...</span></div>
                <div style="clear: both;"></div>
            </div>
            <button class="btn-off" onclick="clearTarget()" style="margin-top: 10px; background-color: #95a5a6; box-shadow: 0 3px 0 #7f8c8d;">Rotayı Temizle / İptal Et</button>
            <button class="btn-motor" id="start-route-btn" onclick="startRouteFollowing()">ROTAYI TAKİP ET</button>
        </div>

        <div class="info-box">
            <div>Araç İçindeki Sensör Durumları</div>
            <br />
            <div style="display:flex; justify-content: space-around; text-align: center; gap: 5px;">
                <div style="width: 33%;">
                    <div style="font-size: 0.9em; font-weight: normal; color: #7f8c8d;">Gaz Sensör 1</div>
                    <div id="gas-display-1" style="font-weight:bold; color:#2c3e50; min-height: 40px;">Yükleniyor...</div>
                </div>
                <div style="width: 33%;">
                    <div style="font-size: 0.9em; font-weight: normal; color: #7f8c8d;">Hava Kalitesi 2</div>
                    <div id="gas-display-2" style="font-weight:bold; color:#2c3e50; min-height: 40px;">Yükleniyor...</div>
                </div>
                <div style="width: 33%;">
                    <div style="font-size: 0.9em; font-weight: normal; color: #7f8c8d;">Nem ve Sıcaklık</div>
                    <div id="gas-display-3" style="font-weight:bold; color:#2c3e50; min-height: 40px;">Yükleniyor...</div>
                </div>
            </div>
        </div>
        <div class="info-box">
            <div style="font-weight:bold; margin-bottom:10px; color: #2980b9;">Manyetik Alan</div>
            <div style="display:flex; justify-content: space-around; text-align: center; gap: 5px;">
                <div style="width: 25%;">
            <div style="font-size: 0.8em; color: #7f8c8d;">Mag-X</div>
            <div id="mag-x-val" style="font-weight:bold;">0</div>
                </div>
                <div style="width: 25%;">
                    <div style="font-size: 0.8em; color: #7f8c8d;">Mag-Y</div>
                    <div id="mag-y-val" style="font-weight:bold;">0</div>
                </div>
                <div style="width: 25%;">
                    <div style="font-size: 0.8em; color: #7f8c8d;">Mag-Z</div>
                    <div id="mag-z-val" style="font-weight:bold;">0</div>
                </div>
                <div style="width: 25%; background-color: #f1f1f1; border-radius: 5px; padding: 2px;">
                    <div style="font-size: 0.8em; color: #2c3e50; font-weight: bold;">Toplam Şiddet</div>
                    <div id="mag-total-val" style="font-weight:bold; color: #e67e22;">0 uT</div>
                </div>
            </div>
        </div>

        <div class="info-box" id="water-sample-panel" style="display:none;">
            <div style="font-weight:bold; margin-bottom:10px;">Deniz Suyu Analizi</div>
            <div style="display:flex; justify-content: space-around; text-align:center; gap:5px;">
                <div style="width: 33%;">
                    <div style="font-size:0.9em; color:#7f8c8d;">İletkenlik (TDS)</div>
                    <div id="tds-val" style="font-weight:bold; min-height:40px;">0 ppm</div>
                </div>
                <div style="width: 33%;">
                    <div style="font-size:0.9em; color:#7f8c8d;">Su Sıcaklığı</div>
                    <div id="water-temp-val" style="font-weight:bold; min-height:40px;">0 °C</div>
                </div>
                <div style="width: 33%;">
                    <div style="font-size:0.9em; color:#7f8c8d;">Su Kalitesi</div>
                    <div id="water-quality-val" style="font-weight:bold; min-height:40px;">-</div>
                </div>
                
            </div>
            <div style="display:flex; gap:10px; margin-top:15px;">
                <button style="background-color: #2980b9; box-shadow: 0 3px 0 #1c5980; width: 33%; color:white;" onclick="saveToFirebase()">VERİLERİ VERİTABANINA KAYDET</button>
                <!-- <button class="btn-warning" style="flex:1; width: 33%;" onclick="drainWater()">HAZNEYİ BOŞALT</button> -->
                <button class="btn-secondary" style="flex:1; width:33%;" onclick="hideWaterPanel()">BURAYI GİZLE</button>
            </div>
            </div>
            
        </div>
    </div>
</div>
    <script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script>
    var map, marker;
    var geofenceCircle = null;
    var routeLine;
    var manualRouteLine;
    var routeWaypoints = [];
    var waypointMarkers;
    var targetDistanceDiv, targetETADiv, requiredFuelDiv, distanceInfoBox;
    var coordDiv, datetimeDiv, speedHeadingDiv;
    var wifiStatusSpan, gsmInfoSpan, gpsFixInfoSpan, batteryInfoSpan;

    // KONUM TAKİBİ DEĞİŞKENLERİ
    var followEnabled = true;
    var isUserInteracting = false;

    // GPS STABILIZATION
    let smoothLat = null;
    let smoothLon = null;
    let lastGpsLat = null;
    let lastGpsLon = null;

    const SMOOTH_ALPHA = 0.9;
    const MIN_MOVE_METERS = 5;
    const FOLLOW_MOVE_METERS = 20;

    // BAŞLANGIÇ NOKTASI
    let homePosition = null;
    let motorRunning = false;
    let currentCommand = "X";
    let isFetching = false;

    const FRIGATE_IP = "/frigate-api"; 
    const CAMERA_NAME = "c500";
    const GO2RTC_API = "/live/api/streams";

    // DOM Önbelleğe Alma
    let serialLogDiv, motorBtn, followToggleBtn, setHomeBtn, returnBtn, startRouteBtn;
    let waterPanel, waterToggleBtn, tdsVal, waterTempVal, waterQualityVal;
    let airSensDiv, gasValDiv, gasStatusDiv;
    let magX, magY, magZ, magTotal;

    function logSerial(message) {
        if (!serialLogDiv) serialLogDiv = document.getElementById("serial-log");
        const now = new Date();
        const timeStamp = `[${String(now.getHours()).padStart(2, "0")}:${String(now.getMinutes()).padStart(2, "0")}:${String(now.getSeconds()).padStart(2, "0")}] `;
        serialLogDiv.innerText += timeStamp + message + "\n";
        serialLogDiv.scrollTop = serialLogDiv.scrollHeight;
    }

    function toggleMotor() {
        motorRunning = !motorRunning;

        if (motorRunning) {
            motorBtn.innerText = "MOTORLARI DURDUR";
            motorBtn.style.backgroundColor = "#27ae60";
            motorBtn.style.boxShadow = "0 3px 0 #1e8449";
            logSerial("Motor Kilidi Açıldı. Kontrol Aktif.");
        } else {
            motorBtn.innerText = "MOTORLARI ÇALIŞTIR";
            motorBtn.style.backgroundColor = "#e74c3c";
            motorBtn.style.boxShadow = "0 3px 0 #c0392b";
            fetch("/key?value=X");
            logSerial("Motorlar Kilitli.");
        }
    }

    function lockMotorsEmergency() {
        motorRunning = false;
        motorBtn.innerText = "MOTORLARI ÇALIŞTIR";
        motorBtn.style.backgroundColor = "#e74c3c";
        motorBtn.style.boxShadow = "0 3px 0 #c0392b";

        fetch("/key?value=X");
        currentCommand = "X"; 
        logSerial("Geofence: Motorlar Güvenlik Amacıyla Kilitlendi.");
    }

    function sendLocationSms() {
        const phone = "5510620766";

        logSerial("SMS Gönderiliyor → " + phone + " ...");
    
        fetch(`/sendsms?phone=${encodeURIComponent(phone)}`)
            .then(r => r.ok ? r.json() : Promise.reject("Sunucu Hatası"))
            .then(data => {
                const statusMap = {
                    "QUEUED": "✅ SMS başarıyla kuyruğa alındı! (" + phone + ")",
                    "GPS_NOT_READY": "❌ GPS henüz hazır değil!"
                };
                logSerial(statusMap[data.status] || "SMS durumu: " + data.status);
            })
            .catch(err => logSerial("SMS gönderilirken hata: " + err));
    }

    function distanceMeters(a, b) {
        return map.distance(a, b);
    }

    document.addEventListener("DOMContentLoaded", function () {
        // DOM Elementlerini bir kez hafızaya alıyoruz (Performans için)
        serialLogDiv = document.getElementById("serial-log");
        motorBtn = document.getElementById("motor-btn");
        followToggleBtn = document.getElementById("follow-toggle");
        setHomeBtn = document.getElementById("set-home-btn");
        returnBtn = document.getElementById("return-btn");
        startRouteBtn = document.getElementById("start-route-btn");
        
        waterPanel = document.getElementById("water-sample-panel");
        waterToggleBtn = document.getElementById("water-toggle-btn");
        tdsVal = document.getElementById("tds-val");
        waterTempVal = document.getElementById("water-temp-val");
        waterQualityVal = document.getElementById("water-quality-val");
        
        airSensDiv = document.getElementById("gas-display-3");
        gasValDiv = document.getElementById("gas-display-1");
        gasStatusDiv = document.getElementById("gas-display-2");
        
        magX = document.getElementById("mag-x-val");
        magY = document.getElementById("mag-y-val");
        magZ = document.getElementById("mag-z-val");
        magTotal = document.getElementById("mag-total-val");

        coordDiv = document.getElementById('coords');
        datetimeDiv = document.getElementById('datetime-display');
        speedHeadingDiv = document.getElementById('speed-heading-display');

        wifiStatusSpan = document.getElementById('wifi-status');
        gsmInfoSpan = document.getElementById('gsm-info');
        gpsFixInfoSpan = document.getElementById('gps-fix-info');
        batteryInfoSpan = document.getElementById('battery-info');

        targetDistanceDiv = document.getElementById('target-distance');
        targetETADiv = document.getElementById('target-eta');
        requiredFuelDiv = document.getElementById('required-fuel');
        distanceInfoBox = document.getElementById('route-summary-info');

        /* HARİTA BAŞLATMA */
        map = L.map('map').setView([41.0082, 28.9784], 13);

        var osm = L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', { maxZoom: 19 });
        var satellite = L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}', { maxZoom: 19 }).addTo(map);

        L.control.layers({ "Normal Harita": osm, "Uydu Görüntüsü": satellite }).addTo(map);

        marker = L.marker([41.0082, 28.9784], {
            icon: L.divIcon({
                className: 'ship-marker-container',
                html: '<div id="ship-icon" style="font-size:32px; transform:rotate(0deg); transition:transform 0.3s;">⬆️</div>',
                iconSize: [32, 32],
                iconAnchor: [16, 16]
            })
        }).addTo(map);

        routeLine = [];
        manualRouteLine = L.polyline([], { color: '#1565c0', weight: 5, dashArray: '5,5' }).addTo(map);
        waypointMarkers = L.layerGroup().addTo(map);

        /* ROTA ÇİZİMİ */
        map.on('click', function (e) {
            routeWaypoints.push(e.latlng);
            let index = routeWaypoints.length;
            L.marker(e.latlng, {
                icon: L.divIcon({
                    className: 'waypoint-marker-container',
                    html: `<div>${index}</div>`,
                    iconSize: [28, 28],
                    iconAnchor: [14, 28]
                })
            }).addTo(waypointMarkers);

            updateRouteDisplayAndCalculation();
        });

        map.on('dragstart', function () {
            if (followEnabled) {
                isUserInteracting = true;
                followEnabled = false;
                followToggleBtn.innerText = "KONUM TAKİBİ: KAPALI";
            }
        });

        // Fonksiyonları başlat
        updateRealTime();
        updateConnectionType();
        setInterval(measureLatency, 5000);
        updateAllData();
    });

    // SAAT GÜNCELLEMESİ
    function updateRealTime() {
        const now = new Date();
        datetimeDiv.innerHTML = `${now.toLocaleDateString('tr-TR')} ${now.toLocaleTimeString('tr-TR')}`;
        requestAnimationFrame(updateRealTime);
    }

    function updateConnectionType() {
        const connection = navigator.connection || navigator.mozConnection || navigator.webkitConnection;
        document.getElementById("camera-connection").innerText = connection ? connection.effectiveType.toUpperCase() : "LAN";
    }

    function measureLatency() {
        const start = performance.now();
        const latencyDiv = document.getElementById("camera-latency");
        fetch("/live/api") 
            .then(res => res.ok ? (latencyDiv.innerText = Math.round(performance.now() - start) + " ms") : Promise.reject())
            .catch(() => latencyDiv.innerText = "---");
    }

    function toggleFollow() {
        followEnabled = !followEnabled;
        followToggleBtn.innerText = "KONUM TAKİBİ: " + (followEnabled ? "AÇIK" : "KAPALI");

        if (followEnabled) {
            map.panTo(marker.getLatLng(), { animate: true, duration: 0.4 });
        }
    }

    function calculateDistance(lat1, lon1, lat2, lon2) {
        const R = 6371000;
        const dLat = (lat2 - lat1) * Math.PI / 180;
        const dLon = (lon2 - lon1) * Math.PI / 180;
        const a = Math.sin(dLat / 2) ** 2 + Math.cos(lat1 * Math.PI / 180) * Math.cos(lat2 * Math.PI / 180) * Math.sin(dLon / 2) ** 2;
        return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
    }

    function updateRouteDisplayAndCalculation() {
        if (routeWaypoints.length === 0) {
            manualRouteLine.setLatLngs([]);
            clearTarget();
            return;
        }

        let shipPos = marker.getLatLng(); 
        let totalDistance = 0;
        let prev = shipPos;
        let allPoints = [shipPos];

        routeWaypoints.forEach(p => {
            allPoints.push(p);
            totalDistance += calculateDistance(prev.lat, prev.lng, p.lat, p.lng);
            prev = p;
        });

        manualRouteLine.setLatLngs(allPoints);

        let km = totalDistance / 1000;
        let etaMin = (km / 10) * 60;
        let fuel = km * 0.5;

        targetDistanceDiv.innerHTML = km.toFixed(2) + " km";
        targetETADiv.innerHTML = `${Math.floor(etaMin / 60)} sa ${Math.round(etaMin % 60)} dk`;
        requiredFuelDiv.innerHTML = fuel.toFixed(2) + " L";

        distanceInfoBox.style.display = "block";
    }

    function clearTarget() {
        waypointMarkers.clearLayers();
        routeWaypoints = [];
        manualRouteLine.setLatLngs([]);
        distanceInfoBox.style.display = "none";

        if (startRouteBtn) startRouteBtn.style.display = "block";

        if (returnBtn && (returnBtn.innerText.includes("DÖNÜŞ") || returnBtn.innerText.includes("DÖNÜYOR"))) {
            returnBtn.innerText = "BAŞLANGIÇ NOKTASINA DÖN";
            returnBtn.style.backgroundColor = "#f39c12"; 
            returnBtn.style.boxShadow = "0 3px 0 #e67e22";
            returnBtn.style.color = "#fff";
        }
        logSerial("Görev İptal Edildi ve Rota Temizlendi.");
    }

    function geofenceControl() {
        var radiusInput = parseFloat(document.getElementById('geofence-radius').value);
        if (isNaN(radiusInput) || radiusInput <= 0) {
            alert("Lütfen geçerli bir yarıçap girin! (Minimum 5 metre)");
            return;
        }

        let center = homePosition || marker.getLatLng();

        if (geofenceCircle) {
            // --- DEVRE DIŞI BIRAK ---
            map.removeLayer(geofenceCircle);
            geofenceCircle = null;

            fetch(`/setgeofence?enabled=0`)
                .then(() => logSerial("Geofence devre dışı bırakıldı."))
                .catch(() => logSerial("Geofence kapatma isteği iletilemedi."));
        } 
        else {
            // --- ETKİNLEŞTİR ---
            geofenceCircle = L.circle(center, {
                color: 'red',
                fillColor: '#f03',
                fillOpacity: 0.2,
                radius: radiusInput,
                className: 'geofence-circle'
            }).addTo(map);

            // Sunucuya dinamik güncelleme gönder
            fetch(`/setgeofence?enabled=1&radius=${radiusInput}&lat=${center.lat}&lon=${center.lng}`)
                .then(r => r.json())
                .then(data => {
                    logSerial(`Geofence AKTİF → Yarıçap: ${data.radius} m | Merkez: ${center.lat.toFixed(5)}, ${center.lng.toFixed(5)}`);
                })
                .catch(() => logSerial("Geofence ayarlama isteği iletilemedi."));
        }
    }

    /* KLAVYE KONTROLLERİ */
    const keys = { w: false, s: false, a: false, d: false, q: false, e: false, x: false };

    window.addEventListener("keydown", (e) => {
        const key = e.key.toLowerCase();
        if (keys.hasOwnProperty(key) && !keys[key]) {
            keys[key] = true;
            checkKeys();
            e.preventDefault();
        }
    });

    window.addEventListener("keyup", (e) => {
        const key = e.key.toLowerCase();
        if (keys.hasOwnProperty(key)) {
            keys[key] = false;
            checkKeys();
            e.preventDefault();
        }
    });

    function checkKeys() {
        if (!motorRunning) {
            if (keys.w || keys.s || keys.a || keys.d) logSerial("Motorlar Kilitli. Komut gönderilemez.");
            return; 
        }

        let command = "X";
        if (keys.w) command = "W";
        else if (keys.s) command = "S";
        else if (keys.a) command = "A";
        else if (keys.d) command = "D";
        else if (keys.x) command = "X";

        if (command !== currentCommand) {
            currentCommand = command;
            fetch(`/key?value=${currentCommand}`)
                .then(() => logSerial("Komut Gönderildi: " + currentCommand))
                .catch(() => logSerial("Hata: Komut iletilemedi"));
        }
    }

    function showWaterPanel() {
        waterPanel.style.display = "block";
        waterToggleBtn.innerText = "SU DEĞERİNİ GÖRÜNTÜLE";
        logSerial("Su numunesi alınıyor...");

        fetch('/pompala')
            .then(r => r.json())
            .then(data => logSerial(data.status === "OK" ? "Numune Alındı." : "Hata: Pompa başlatılamadı."))
            .catch(err => logSerial("Bağlantı hatası: Pompa komutu iletilemedi."));
    }

    function hideWaterPanel() { waterPanel.style.display = "none"; }

    function drainWater() {
        waterPanel.style.display = "none";
        waterToggleBtn.innerText = "SU NUMUNESİ AL";
    }

    function setHomePosition() {
        if (!marker) return logSerial("Konum bilgisi yok, başlangıç noktası belirlenemedi!");

        homePosition = marker.getLatLng();
        setHomeBtn.innerText = "BAŞLANGIÇ NOKTASI BELİRLENDİ";
        setHomeBtn.style.backgroundColor = "#27ae60";
        logSerial(`Başlangıç noktası belirlendi: ${homePosition.lat.toFixed(5)}, ${homePosition.lng.toFixed(5)}`);

        L.marker(homePosition, {
            icon: L.divIcon({
                className: 'home-marker',
                html: '<div style="font-size:24px; color:blue;">🏠</div>',
                iconSize: [24, 24], iconAnchor: [12, 24]
            })
        }).addTo(map).bindPopup("Başlangıç Noktası");
    }

    function returnToHome() {
        if (!homePosition) return logSerial("Başlangıç noktası belirlenmedi, dönüş yapılamıyor!");

        returnBtn.innerText = "GEMİ DÖNÜŞ ROTASINDA";
        returnBtn.style.backgroundColor = "#c0392b";
        returnBtn.style.boxShadow = "0 3px 0 #922b21";

        distanceInfoBox.style.display = "block";
        targetDistanceDiv.innerHTML = distanceMeters(marker.getLatLng(), homePosition).toFixed(0) + " metre";
        targetETADiv.innerHTML = "Hesaplanıyor...";
        requiredFuelDiv.innerHTML = "Hesaplanıyor...";
    }

    function startRouteFollowing() {
        if (startRouteBtn) startRouteBtn.style.display = "none";
        logSerial("Rota Takip Ediliyor...");
    }

    /* ANA VERİ DÖNGÜSÜ */
    function updateAllData() {
        if (isFetching) return;
        isFetching = true;

        fetch('/api/all')
            .then(r => r.ok ? r.json() : Promise.reject("Bağlantı Hatası"))
            .then(data => {
                if (data.geofence_enabled !== undefined) {
                    geofenceEnabled = data.geofence_enabled;
                }

                let lat = parseFloat(data.lat);
                let lon = parseFloat(data.lon);
                let speed = parseFloat(data.speed) || 0;
                let heading = parseFloat(data.head) || 0;

                gpsFixInfoSpan.innerHTML = data.gps_status || "Bilinmiyor";

                // 1. KONUM VE HARİTA İŞLEMLERİ
                if (!isNaN(lat) && !isNaN(lon) && lat !== 0 && lon !== 0) {
                    
                    // Geofence Mantığı
                    if (geofenceCircle) {
                        let currentPos = L.latLng(lat, lon); 
                        let distanceToCenter = distanceMeters(currentPos, geofenceCircle.getLatLng());
                        let allowedRadius = geofenceCircle.getRadius();

                        if (distanceToCenter >= (allowedRadius - 5)) {
                            geofenceCircle.setStyle({color: '#ff0000', fillColor: '#ff0000', fillOpacity: 0.4});
                            logSerial(`UYARI: Geofence sınırına ${(distanceToCenter - allowedRadius + 5).toFixed(1)} m kaldı! Sistem geri çekiyor...`);
                        } else {
                            geofenceCircle.setStyle({color: '#27ae60', fillColor: '#2ecc71', fillOpacity: 0.2});
                        }
                    }

                    // Kalman filter / Smooth GPS
                    if (smoothLat === null) { smoothLat = lat; smoothLon = lon; } 
                    else {
                        smoothLat += SMOOTH_ALPHA * (lat - smoothLat);
                        smoothLon += SMOOTH_ALPHA * (lon - smoothLon);
                    }

                    let pos = L.latLng(smoothLat, smoothLon);
                    
                    if (distanceMeters(marker.getLatLng(), pos) >= MIN_MOVE_METERS) {
                        marker.setLatLng(pos);

                        if (followEnabled && distanceMeters(map.getCenter(), pos) > FOLLOW_MOVE_METERS) {
                            map.panTo(pos, { animate: true, duration: 0.5 });
                        }

                        if (routeWaypoints.length > 0) updateRouteDisplayAndCalculation();
                    }

                    const shipIcon = document.getElementById("ship-icon");
                    if (shipIcon) shipIcon.style.transform = `rotate(${heading}deg)`;

                    coordDiv.innerHTML = `${smoothLat.toFixed(5)}, ${smoothLon.toFixed(5)}`;
                }

                // 2. SİSTEM VE SENSÖR BİLGİLERİ
                speedHeadingDiv.innerHTML = `Hız: ${speed.toFixed(1)} km/h<br>Yön: ${heading.toFixed(1)}°`;
                wifiStatusSpan.innerHTML = "Bağlı (VPN)"; 
                gsmInfoSpan.innerHTML = data.gsm_signal || "Yok";
                batteryInfoSpan.innerHTML = `${data.batt || 0}%`;

                // Su Kalitesi
                if (waterPanel.style.display !== "none") {
                    tdsVal.innerText = `${data.tds || 0} ppm`;
                    waterTempVal.innerText = `${data.water_temp || 0} °C`;
                    waterQualityVal.innerText = data.quality || "Bilinmiyor";
                    waterQualityVal.style.color = data.tds > 500 ? "#e74c3c" : (data.tds > 170 ? "#f39c12" : "#27ae60");
                }
                
                // Hava Sensörü
                if (data.air_temp !== undefined) {
                    airSensDiv.innerHTML = `<div><span style="color: #e67e22; font-weight: bold;">🌡️ ${parseFloat(data.air_temp).toFixed(1)}°C</span></div>
                                            <div><span style="color: #3498db; font-weight: bold;">💧 %${Math.round(data.air_hum)}</span></div>`;
                }

                // Gaz Sensörü
                if (data.gas_val !== undefined) {
                    gasValDiv.innerHTML = `<span style="font-size:1.2em;">%${((data.gas_val/1024)*100).toFixed(1)}</span><br><small>Analog: ${data.gas_val}</small>`;
                    if (data.gas_alarm == 1 || data.gas_val > 400) {
                        gasStatusDiv.innerHTML = "<span style='color:#e74c3c; font-weight:bold;'>!!! TEHLİKE !!!</span>";
                        if (data.gas_alarm == 1) logSerial("KRİTİK: Gaz Sızıntısı Algılandı!");
                    } else {
                        gasStatusDiv.innerHTML = "<span style='color:#27ae60;'>TEMİZ</span>";
                    }
                }

                // Manyetometre
                if (data.mag_total !== undefined) {
                    magX.innerText = data.mag_x; magY.innerText = data.mag_y; magZ.innerText = data.mag_z;
                    magTotal.innerText = data.mag_total + " uT";
                    magTotal.style.color = data.mag_total > 100 ? "#e74c3c" : "#e67e22";
                }
            })
            .catch(err => wifiStatusSpan.innerHTML = "<span style='color:red'>HATA</span>")
            .finally(() => {
                isFetching = false;
                setTimeout(updateAllData, 1000);
            });
    }

    function setSystemMode(mode) {
        currentMode = mode;
        logSerial("Sistem modu değiştirildi: " + mode);
        // Arka plana da bildir (isteğe bağlı)
        fetch('/api/mode?value=' + mode)
            .catch(() => logSerial("Mod bilgisi ESP'ye iletilemedi."));

        // Kullanıcıya görsel feedback
        if (mode === "ACİL") {
            logSerial("⚠️ ACİL DURUM MODU AKTİF - Motorlar hazırda bekletiliyor!");
        }
    }
</script>
</body>
</html>
)rawliteral";