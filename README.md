# 🚢 AVCI - İnsansız Deniz Aracı (İDA)

Deniz kirliliğiyle mücadele, illegal sintine/atık deşarjlarının takibi ve çevre denetimi için tasarlanmış; otonom navigasyon yeteneğine sahip, düşük maliyetli ve yüksek verimli bir İnsansız Deniz Aracı prototipidir. 

🏆 **Başarı:** Projemiz, Birixa Ulusal Robot Yarışması Serbest Kategori'de **1.'lik ödülüne** layık görülmüştür.

---

## 📑 İçindekiler
- [Projenin Amacı ve Çözümü](#-projenin-amacı-ve-çözümü)
- [Gövde ve Tahrik Yapısı](#-gövde-ve-tahrik-yapısı)
- [Donanım Mimarisi](#-donanım-mimarisi-ve-görev-dağılımı)
- [Ağ ve Telemetri Sistemi](#-ağ-ve-telemetri-sistemi)
- [Sensör ve Analiz Yetenekleri](#-sensör-ve-analiz-yetenekleri)
- [Klasör ve Dosya Yapısı](#-klasör-ve-dosya-yapısı)
- [Kurulum ve Yükleme Adımları](#-kurulum-ve-yükleme-adımları)

---

## 🎯 Projenin Amacı ve Çözümü
Denizlerdeki kirliliği anlık ve yerinde tespit edebilmek geleneksel yöntemlerle oldukça maliyetli ve yavaştır. AVCI, sahip olduğu sensör ağı ve izole su analiz haznesiyle bu süreci otonom hale getirir. 

> **💡 Mühendislik Çözümü (Neyi Çözdük?):**
> İnsansız deniz araçlarında en büyük sorun standart RF kumandaların yarattığı mesafe sınırıdır. AVCI'da bu sınır; **4G LTE Modem, Raspberry Pi ve VPN Server** tabanlı özel bir ağ mimarisi kurgulanarak tamamen ortadan kaldırılmıştır. Araç, internetin çektiği her yerden kontrol edilebilir ve anlık telemetri aktarabilir.

---

## 📐 Gövde ve Tahrik Yapısı
* **Gövde Tasarımı:** Su direncini minimize eden, hidrodinamik açıdan avantajlı, ince ve uzun form.
* **Tahrik Sistemi:** Arkada çift motorlu diferansiyel sürüş (tank dönüş mantığı).
* **Yönlendirme:** Fiziksel bir dümen palası **yoktur**. Manevralar, motorların bağımsız hız farklarıyla (PWM) sağlanır.
* **Tork Dengeleme:** Motor torkunun aracı saptırmasını engellemek için zıt yönlü (CW/CCW - Saat Yönü ve Tersi) pervane seti kullanılmıştır.

---

## ⚙️ Donanım Mimarisi ve Görev Dağılımı

| Birim | Donanım | Görevi |
| :--- | :--- | :--- |
| **Ana Beyin** | ESP8266 | WEB arayüzünü barındırır. Motor sürücülerine PWM sinyali gönderir. Mega ve GSM'den gelen verileri işleyip arayüze aktarır. |
| **İşçi Beyin** | Arduino Mega | Sensör verilerini toplar ve ESP'ye iletir. ESP'den gelen komutla su numune pompasını çalıştırır. |
| **Ağ Yöneticisi** | Raspberry Pi 4B (4GB) | RTSP kamera akışını alır, yerel ağda ESP ile haberleşir ve VPN sunucusuna bağlanarak sisteme uzaktan erişim sağlar. |
| **Ana Güç** | 12.8V 7200mAh LiFePO4 Akü | Yüksek akım gerektiren motor sürücülerini, kamerayı, GSM Shield'ı ve su pompasını besler. |
| **Kontrol Gücü** | 5V Powerbank | Voltaj dalgalanmalarından korumak amacıyla ESP8266, Arduino Mega ve tetikleyici sürücüleri besler. |

---

## 📡 Ağ ve Telemetri Sistemi
Sistem iletişimi üç temel aşamadan oluşur:
1. **Yerel İletişim:** 4G LTE Modem; kamera, ESP8266 ve Raspberry Pi'yi aynı yerel ağda birleştirir.
2. **Uzak Bağlantı (VPN):** Raspberry Pi üzerinden VPN Server'a tünel açılır. Operatör, dünyanın herhangi bir yerinden `192.168.99.3` IP adresine giderek araca bağlanır.
3. **Telemetri & Güvenlik:** SIM808 GSM Shield, şebeke üzerinden konum koordinatlarını alır, web arayüzündeki haritaya basar ve acil durumlarda operatöre SMS ile konum gönderir.

---

## 🧪 Sensör ve Analiz Yetenekleri
AVCI, çevresel verileri toplamak için gelişmiş bir sensör katarına sahiptir.

* **İzole Analiz Haznesi:** Motorların yarattığı türbülanstan (köpük ve hava kabarcıkları) etkilenmemesi için teknenin orta-kıç kısmında tabana sıfır konumlandırılmış özel bir oda.
* **Su Kalitesi (TDS & DS18B20):** İzole haznede alınan suyun TDS (Toplam Çözünmüş Katı Madde) ve sıcaklık değerleri ölçülür. Yazılımsal sıcaklık kompanzasyonu ve median (ortanca) filtreleme ile **%95'in üzerinde doğruluk** elde edilir.
* **Yön ve Manyetik Alan (9 Eksenli IMU):** Aracın pusula yönü (heading) ve çevresel manyetik alan bozulmaları ölçülür.
* **Hava Kalitesi:** MQ9 sensörü ile tehlikeli/yanıcı gaz tespiti; DHT11 ile ortam sıcaklığı ve nem ölçümü yapılır.

---

## 📂 Klasör ve Dosya Yapısı
Depodaki kaynak kodlar ve ayar dosyaları aşağıdaki mimariye göre organize edilmiştir:

```text
AvciGemi/
├── README.md                           # Proje dokümantasyonu
├── src/
│   ├── esp8266/
│   │   └── 6_04_2026_ESP.ino           # Ana kontrol ve Web Server kodu
│   └── arduino_mega/
│       └── 6_04_2026_sensorKodlar.ino  # Sensör okuma ve pompa kontrol kodu
└── config/
    ├── config_yml.yaml                 # Raspberry Pi go2rtc kamera akış ayarları
    ├── default.conf                    # Raspberry Pi Nginx reverse proxy ayarları
    └── gemi.ovpn                       # Raspberry Pi VPN istemci yapılandırması