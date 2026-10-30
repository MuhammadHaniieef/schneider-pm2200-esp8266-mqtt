<div align="center">

# ⚡ PM2200 · ESP8266 · MQTT Ref5

**Monitoring listrik tiga fasa dengan pembacaan dua blok register dan validasi CRC**

![ESP8266](https://img.shields.io/badge/Platform-ESP8266-00979D?style=for-the-badge)
![Schneider](https://img.shields.io/badge/Meter-Schneider_PM2200-3DCD58?style=for-the-badge)
![Modbus](https://img.shields.io/badge/Modbus-RTU_RS485-orange?style=for-the-badge)
![MQTT](https://img.shields.io/badge/Telemetry-MQTT_JSON-660066?style=for-the-badge)

Gateway Schneider Electric PM2200 ke MQTT menggunakan Wemos ESP8266.
Membaca arus, tegangan, daya, dan faktor daya, lalu menerbitkan satu payload hanya setelah kedua grup register berhasil dibaca.

</div>

---

## ✨ Apa yang berbeda pada Ref5?

| Aspek | Proyek hybrid sebelumnya | Ref5 |
| --- | --- | --- |
| Strategi pembacaan | Enam langkah bergantian | Dua blok register berturutan |
| CRC permintaan | Disimpan dalam frame tetap | Dihitung otomatis |
| Validasi respons | Pemeriksaan terbatas | Alamat slave, fungsi, byte count, dan CRC |
| Saat pembacaan gagal | Nilai lama dapat tetap diterbitkan | Siklus dibatalkan jika salah satu grup gagal |
| Parameter tambahan | Arus, tegangan L-N, daya aktif total | Tegangan L-L, daya aktif per fasa, daya reaktif/semu total, PF |
| Diagnostik payload | Tanpa penghitung siklus | `seq`, `err`, dan `read_ms` |
| Web server / OTA | Sinkron, ElegantOTA v3 | Asinkron, AsyncElegantOTA |
| Pin RS485 | RX D1, TX D5, DE/RE D6 | **RX D7, TX D6, DE/RE D5** |

> Wiring berbeda dari proyek hybrid sebelumnya. Periksa pin sebelum memasang perangkat.

## 🔄 Alur kerja

```text
PM2200 ── RS485 / Modbus RTU ── Transceiver ── ESP8266
                                                   │ Wi-Fi
                                                   ▼
                                              MQTT Broker
                                                   │
                                                   ▼
                                           Subscriber / Dashboard
```

1. Kirim permintaan grup 1: alamat awal `0x0BB7`, **38 register**, respons **81 byte**.
2. Validasi respons, dengan satu kali retry jika gagal.
3. Tunggu sekitar **15 ms**, lalu baca grup 2: alamat awal `0x0BED`, **32 register**, respons **69 byte**.
4. Jika salah satu grup gagal setelah retry, tambah penghitung error dan batalkan publikasi.
5. Jika keduanya berhasil, susun JSON dan coba publikasikan ke MQTT.

Target siklus adalah **1000 ms**, bukan jaminan real-time. Durasi respons, retry, dan koneksi memengaruhi waktu aktual. Kedua grup dibaca berurutan, bukan pengukuran serentak.

## 🔌 Wiring

| Transceiver RS485 | Wemos | Fungsi |
| --- | --- | --- |
| RO | D7 / GPIO13 | RX |
| DI | D6 / GPIO12 | TX |
| DE dan /RE digabung | D5 / GPIO14 | Kontrol arah |
| GND | GND | Referensi ground sisi logika |
| VCC | Sesuai spesifikasi modul | Catu daya |

Hubungkan A/B ke terminal RS485 meter sesuai manual. Gunakan transceiver berlogika **3,3 V** atau level shifter yang sesuai: ESP8266 tidak toleran terhadap 5 V.

**Pengaturan komunikasi meter:** alamat slave `1`, **19200 baud, 8N1**. Alamat slave dapat diubah melalui `SLAVE_ADDR` pada sketch.

> ⚠️ Pekerjaan panel listrik harus dilakukan personel yang kompeten. Jangan hubungkan tegangan listrik utama ke pin Wemos. Firmware ini bukan sistem proteksi listrik.

## 🚀 Instalasi

### Library

- Board package **ESP8266** untuk Arduino IDE.
- **SoftwareSerial / EspSoftwareSerial** yang kompatibel dengan core ESP8266.
- **PubSubClient >= 2.8** untuk `setBufferSize()`.
- **ESPAsyncTCP**, **ESPAsyncWebServer**, dan **AsyncElegantOTA** yang saling kompatibel.
- **LittleFS** dari board package ESP8266.

> Sketch menggunakan API lama `AsyncElegantOTA.h`. Library ini sudah deprecated; jangan menggantinya langsung dengan ElegantOTA v3 tanpa menyesuaikan kode dan mode async. Versi dependency belum dikunci di repository ini, jadi kompatibilitas perlu diverifikasi saat kompilasi.

### Langkah awal

1. Tempatkan sketch dalam folder bernama **`pm2200_mqtt_ref5`**, lalu buka `pm2200_mqtt_ref5.ino`.
2. Instal library dan pilih board ESP8266, port USB, serta konfigurasi flash yang sesuai.
3. Buat konfigurasi lokal:

   ```bash
   cp secrets.example.h secrets.h
   ```

4. Isi Wi-Fi, broker MQTT, username/password, `baseTopic`, dan nama perangkat pada `secrets.h`.
5. Periksa wiring serta pengaturan meter. Cocokkan register dengan model dan manual PM2200 yang digunakan.
6. Upload melalui USB dan buka Serial Monitor pada **115200 baud**.
7. Subscribe ke `<baseTopic>/telemetry` untuk melihat data.

`secrets.h` diabaikan Git. Nilai pada template hanyalah contoh; jangan commit kredensial asli.

## 📡 Topik MQTT

| Topik | Payload | Retained |
| --- | --- | --- |
| `<baseTopic>/telemetry` | JSON pengukuran | Tidak |
| `<baseTopic>/status` | `online` / Last Will `offline` | Ya |
| `<baseTopic>/ip` | IP lokal ESP8266 | Ya |

Buffer PubSubClient disetel ke **1024 byte**, dan buffer JSON juga **1024 byte**. Batas paket MQTT mencakup topik serta header, sehingga payload yang mendekati batas masih dapat gagal dipublikasikan.

### Field telemetri

| Field | Arti |
| --- | --- |
| `seq` | Jumlah siklus dua grup yang berhasil dibaca sejak boot; bukan jumlah publish yang berhasil |
| `err` | Jumlah siklus yang dibatalkan akibat kegagalan pembacaan sejak boot |
| `read_ms` | Durasi pembacaan kedua grup, termasuk jeda dan retry |
| `current_a`, `current_b`, `current_c`, `current_avg` | Arus per fasa dan rata-rata |
| `voltage_ab`, `voltage_bc`, `voltage_ca`, `voltage_ll_avg` | Tegangan antar fasa dan rata-rata |
| `voltage_an`, `voltage_bn`, `voltage_cn`, `voltage_avg` | Tegangan fasa-netral dan rata-rata |
| `active_power_a`, `active_power_b`, `active_power_c`, `active_power` | Daya aktif per fasa dan total |
| `reactive_power_total` | Daya reaktif total |
| `apparent_power_total` | Daya semu total |
| `pf_a`, `pf_b`, `pf_c`, `pf_total` | Faktor daya per fasa dan total |

Nilai `NaN` atau tak hingga diterbitkan sebagai JSON `null`. Periksa satuan daya, konfigurasi CT/PT, dan format PF pada manual serta tampilan meter sebelum memberi label atau membuat perhitungan. Kode mencatat kemungkinan PF empat kuadran (`-2..2`); jangan menganggap semua nilai PF langsung berada pada rentang `-1..1`.

## 🌐 OTA melalui browser

- Buka `http://IP_PERANGKAT/` untuk memeriksa web server.
- Buka `http://IP_PERANGKAT/update` untuk upload firmware `.bin` yang sesuai board dan konfigurasi flash.
- IP ditampilkan di Serial Monitor dan topik MQTT `<baseTopic>/ip` setelah MQTT terhubung.
- Jangan memutus catu daya selama update.

## 🔍 Troubleshooting

| Gejala | Periksa |
| --- | --- |
| Timeout grup G1/G2 | Wiring A/B, DE/RE, catu daya, alamat slave, 19200 baud dan 8N1 |
| CRC respons salah | Gangguan kabel, grounding, terminasi bus, level logika, dan stabilitas SoftwareSerial |
| Fungsi tak terduga / `0x83` | Register atau permintaan ditolak meter; cocokkan register map model yang digunakan |
| Gagal publish | Koneksi/ACL broker, panjang topik + payload, dan buffer MQTT |
| Tidak ada telemetri | Pembacaan hanya dijalankan ketika Wi-Fi dan MQTT terhubung; kedua grup juga harus berhasil |
| LittleFS mount failed | Konfigurasi filesystem/flash; jangan format tanpa memahami data yang akan hilang |

## 🔒 Keamanan dan batasan

- `secrets.h`, folder `build/`, dan binary firmware tidak dilacak Git karena dapat memuat kredensial.
- MQTT menggunakan koneksi tanpa TLS; web/OTA belum memiliki autentikasi pada sketch ini. Gunakan jaringan tepercaya, ACL broker, dan jangan mengeksposnya ke internet.
- CRC memvalidasi integritas frame, bukan keakuratan register map atau ketepatan pengukuran.
- Tidak ada timestamp absolut atau penyimpanan data ketika MQTT terputus.
- Belum ada pengujian hardware otomatis atau penguncian versi library di repository.

---

<div align="center">

**[MuhammadHaniieef](https://github.com/MuhammadHaniieef)** · Schneider PM2200 · ESP8266 · RS485 · MQTT

</div>
