#include <Arduino.h>
#include <HX711_ADC.h>
#include <EEPROM.h>
#include <max6675.h>
#include "KalmanFilter.h"

// Konfigurasi pin HX711 ke Arduino Nano
const int HX711_DOUT_PIN = 4; // Pin D4 (Data/DOUT)
const int HX711_SCK_PIN  = 5; // Pin D5 (Clock/SCK)

// Konfigurasi pin modul termokopel MAX6675 ke Arduino Nano
const int thermoCLK = 6; // Pin D6 (SCK / Clock)
const int thermoCS  = 7; // Pin D7 (CS / Chip Select)
const int thermoDO  = 8; // Pin D8 (SO / Data Out)

// Inisialisasi objek sensor
HX711_ADC LoadCell(HX711_DOUT_PIN, HX711_SCK_PIN);
MAX6675 thermocouple(thermoCLK, thermoCS, thermoDO);

// Filter Kalman Adaptif untuk menstabilkan pembacaan berat (satuan kg)
// Parameter dituning kuat untuk menekan drift pada sensor kapasitas besar (3 Ton):
// q = 0.0001 (process noise), r = 0.20 (measurement noise), threshold = 0.08 kg
KalmanFilter beratKalman(0.0001f, 0.20f, 0.08f);
float displayBerat = 0.0f; // Nilai berat stabil yang ditampilkan ke layar

// Filter Kalman Adaptif untuk menstabilkan pembacaan suhu MAX6675 (satuan Celcius)
// q = 0.02 (proses perubahan suhu), r = 1.50 (meredam loncatan noise ADC 0.25 C), threshold = 2.0 C
KalmanFilter suhuKalman(0.02f, 1.50f, 2.0f);
float displayTempC = 0.0f; // Nilai suhu Celcius stabil
float displayTempF = 0.0f; // Nilai suhu Fahrenheit stabil
bool suhuInitialized = false;

// Alamat EEPROM untuk menyimpan nilai faktor kalibrasi (float = 4 byte)
const int calVal_eepromAddress = 0;

// Variabel waktu untuk interval tampilan di Serial Monitor
unsigned long prevPrintTime = 0;
const unsigned long printInterval = 250; // Tampilkan setiap 250 ms

// Konstanta percepatan gravitasi bumi (m/s^2) untuk konversi massa ke gaya (F = m * g)
const float GRAVITY = 9.80665f;

// Mode toggle untuk menampilkan info Raw ADC / Delta
bool showDebugRaw = false;

// Variabel untuk Automatic Zero Tracking (AZT) dan Stable Lock (Standar Industri)
const float AZT_WINDOW = 0.120f;       // Beban di bawah 120 gram dianggap drift titik nol saat kosong
const float STABLE_TOLERANCE = 0.040f; // Toleransi kestabilan 40 gram
unsigned long lastChangeTime = 0;
bool isWeightLocked = false;

// Deklarasi fungsi
void calibrate();
void changeSavedCalFactor();
void printMenu();

void setup() {
  Serial.begin(57600);
  delay(10);
  Serial.println();
  Serial.println(F("===================================================="));
  Serial.println(F("   Sistem Load Cell & Termokopel MAX6675 Arduino    "));
  Serial.println(F("===================================================="));

  // Menunggu stabilisasi modul MAX6675
  delay(500);

  LoadCell.begin();

  // Membalik arah pembacaan agar gaya tekan (compression pada posisi berdiri) bernilai positif (+)
  LoadCell.setReverseOutput();

  // Membaca faktor kalibrasi dari EEPROM
  float calibrationValue = 0.0;
  EEPROM.get(calVal_eepromAddress, calibrationValue);

  // Periksa apakah nilai kalibrasi di EEPROM valid
  if (isnan(calibrationValue) || isinf(calibrationValue) || calibrationValue == 0.0f) {
    calibrationValue = 1.0f; // Nilai default jika belum pernah dikalibrasi
    Serial.println(F("[INFO] Faktor kalibrasi belum tersimpan di EEPROM. Menggunakan nilai default: 1.0"));
    Serial.println(F("[INFO] Silakan kirim 'r' pada Serial Monitor untuk kalibrasi (satuan: KG)."));
  } else {
    Serial.print(F("[INFO] Faktor kalibrasi dimuat dari EEPROM: "));
    Serial.println(calibrationValue, 2);
  }

  unsigned long stabilizingtime = 2000; // Waktu stabilisasi 2 detik saat startup
  boolean _tare = true;                 // Lakukan tare otomatis saat start
  LoadCell.start(stabilizingtime, _tare);

  if (LoadCell.getTareTimeoutFlag() || LoadCell.getSignalTimeoutFlag()) {
    Serial.println(F("[ERROR] Timeout! Periksa koneksi kabel HX711 dan pinout MCU."));
    while (1);
  } else {
    LoadCell.setCalFactor(calibrationValue);
    Serial.println(F("[STATUS] Inisialisasi selesai. Sensor siap digunakan (Satuan: KG)!"));
  }

  printMenu();
}

void loop() {
  static boolean newDataReady = false;

  // Memeriksa ketersediaan data konversi baru dari HX711 (non-blocking)
  if (LoadCell.update()) {
    newDataReady = true;
  }

  // Tampilkan data beban secara berkala jika data baru sudah tersedia
  if (newDataReady) {
    if (millis() - prevPrintTime >= printInterval) {
      float rawBerat = LoadCell.getData();             // Nilai beban dari moving average HX711_ADC (kg)
      float filteredBerat = beratKalman.update(rawBerat); // Dihaluskan oleh Adaptive Kalman Filter

      // 1. Automatic Zero Tracking (AZT): saat sensor kosong (drift < 120 gram)
      if (fabs(filteredBerat) < AZT_WINDOW) {
        long currentTare = LoadCell.getTareOffset();
        float cal = LoadCell.getCalFactor();
        // Koreksi tareOffset secara bertahap untuk menyerap hanyutan titik nol
        long driftCorrection = (long)(filteredBerat * cal * 0.10f);
        LoadCell.setTareOffset(currentTare + driftCorrection);

        displayBerat = 0.0f;
        isWeightLocked = false;
        lastChangeTime = millis();
      } 
      // 2. Penimbangan Beban Nyata (>= 120 gram):
      else {
        // Cek apakah beban bergerak / berubah melebihi toleransi (40 gram)
        if (fabs(filteredBerat - displayBerat) > STABLE_TOLERANCE) {
          isWeightLocked = false;
          displayBerat = filteredBerat;
          lastChangeTime = millis();
        } else {
          // Beban diam: jika tenang selama 1.2 detik, kunci nilainya agar tidak merayap
          if (!isWeightLocked && (millis() - lastChangeTime >= 1200)) {
            isWeightLocked = true;
            displayBerat = filteredBerat;
          }
          if (!isWeightLocked) {
            displayBerat = filteredBerat;
          }
        }
      }

      float gaya = displayBerat * GRAVITY; // Konversi gaya: F = m * g (Newton)

      // Pembacaan Termokopel MAX6675 (Celcius & Fahrenheit)
      float rawTempC = thermocouple.readCelsius();

      if (!isnan(rawTempC)) {
        if (!suhuInitialized) {
          suhuKalman.reset(rawTempC);
          displayTempC = rawTempC;
          suhuInitialized = true;
        }

        // Saring derau bit ADC 0.25 C dengan Adaptive Kalman Filter
        float filteredTempC = suhuKalman.update(rawTempC);

        // Deadband filter: redam fluktuasi di bawah 0.20 C agar angka display tenang
        if (fabs(filteredTempC - displayTempC) >= 0.20f) {
          displayTempC = filteredTempC;
        }

        // Konversi Fahrenheit presisi dari suhu Celcius yang sudah stabil
        displayTempF = displayTempC * 1.8f + 32.0f;
      }

      Serial.print(F("Berat : "));
      Serial.print(displayBerat, 3);
      Serial.print(F(" kg | Gaya : "));
      Serial.print(gaya, 2);
      Serial.print(F(" N | Suhu : "));

      if (isnan(rawTempC)) {
        Serial.print(F("Probe Terputus!"));
      } else {
        Serial.print(displayTempC, 2);
        Serial.print(F(" C / "));
        Serial.print(displayTempF, 2);
        Serial.print(F(" F"));
      }

      if (isWeightLocked && displayBerat > 0.0f) {
        Serial.print(F(" [STABIL]"));
      }

      if (showDebugRaw) {
        float calFactor = LoadCell.getCalFactor();
        long tareOffset = LoadCell.getTareOffset();
        long rawDelta = (long)(displayBerat * calFactor);
        long rawADC = tareOffset + rawDelta;
        Serial.print(F("  |  Raw ADC: "));
        Serial.print(rawADC);
        Serial.print(F("  |  Delta: "));
        Serial.print(rawDelta);
      }
      Serial.println();

      newDataReady = false;
      prevPrintTime = millis();
    }
  }

  // Menangani input perintah dari Serial Monitor
  if (Serial.available() > 0) {
    char inByte = Serial.read();

    // Abaikan karakter newline dan carriage return
    if (inByte == '\r' || inByte == '\n') {
      return;
    }

    if (inByte == 't') {
      Serial.println(F("-> Melakukan Tare (set titik nol)..."));
      LoadCell.tareNoDelay();
    } else if (inByte == 'r') {
      calibrate();
    } else if (inByte == 'c') {
      changeSavedCalFactor();
    } else if (inByte == 'd') {
      showDebugRaw = !showDebugRaw;
      Serial.print(F("-> Info Raw ADC & Delta: "));
      Serial.println(showDebugRaw ? F("AKTIF") : F("NONAKTIF"));
    } else if (inByte == '?' || inByte == 'h') {
      printMenu();
    }
  }

  // Cek apakah proses Tare non-blocking telah selesai
  if (LoadCell.getTareStatus() == true) {
    beratKalman.reset(0.0f);
    displayBerat = 0.0f;
    isWeightLocked = false;
    lastChangeTime = millis();
    Serial.println(F("[STATUS] Tare selesai. Titik nol berhasil disetel."));
  }
}

/**
 * @brief Prosedur kalibrasi interaktif melalui Serial Monitor.
 * Mengikuti metode standar dari library HX711_ADC (Olav Kallhovd).
 */
void calibrate() {
  Serial.println();
  Serial.println(F("************************************************"));
  Serial.println(F("           MULAI PROSEDUR KALIBRASI             "));
  Serial.println(F("************************************************"));
  Serial.println(F("1. Letakkan load cell pada permukaan yang datar dan stabil."));
  Serial.println(F("2. Pastikan TIDAK ADA BEBAN di atas load cell."));
  Serial.println(F("3. Kirim karakter 't' dari Serial Monitor untuk Tare titik nol..."));

  // Kosongkan sisa buffer serial
  while (Serial.available() > 0) Serial.read();

  // Langkah 1: Menunggu perintah Tare
  boolean _resume = false;
  while (!_resume) {
    LoadCell.update();
    if (Serial.available() > 0) {
      char inByte = Serial.read();
      if (inByte == 't') {
        LoadCell.tareNoDelay();
      }
    }
    if (LoadCell.getTareStatus() == true) {
      Serial.println(F("-> Titik nol (Tare) berhasil disetel!"));
      _resume = true;
    }
  }

  // Langkah 2: Meminta meletakkan beban standar
  Serial.println();
  Serial.println(F("4. Sekarang, letakkan BEBAN YANG SUDAH DIKETAHUI bobotnya di atas load cell."));
  Serial.println(F("5. Masukkan nilai bobot beban tersebut dalam KILOGRAM / KG (contoh: 1.446 atau 50.0) lalu tekan Kirim/Enter:"));

  float known_mass = 0.0;
  _resume = false;
  while (!_resume) {
    LoadCell.update();
    if (Serial.available() > 0) {
      known_mass = Serial.parseFloat();
      if (known_mass != 0.0f) {
        Serial.print(F("-> Bobot beban referensi: "));
        Serial.print(known_mass, 3);
        Serial.println(F(" kg"));
        _resume = true;
      }
    }
  }

  // Berikan waktu stabilisasi mekanik sebelum mengambil data kalibrasi
  Serial.println(F("-> Menunggu stabilisasi beban selama 2 detik..."));
  unsigned long settleStart = millis();
  while (millis() - settleStart < 2000) {
    LoadCell.update();
  }

  // Langkah 3: Menghitung faktor kalibrasi baru
  LoadCell.refreshDataSet(); // Pastikan dataset diperbarui dengan beban terpasang
  float newCalibrationValue = LoadCell.getNewCalibration(known_mass);

  long tareOffset = LoadCell.getTareOffset();
  long rawWithLoad = (long)(LoadCell.getData() * newCalibrationValue) + tareOffset;
  long rawDelta = rawWithLoad - tareOffset;

  Serial.println();
  Serial.print(F("-> Nilai Tare Offset (Kosong) : "));
  Serial.println(tareOffset);
  Serial.print(F("-> Nilai Raw dengan Beban     : "));
  Serial.println(rawWithLoad);
  Serial.print(F("-> Selisih Raw ADC (Delta)    : "));
  Serial.println(rawDelta);
  Serial.print(F("-> Faktor kalibrasi terhitung : "));
  Serial.println(newCalibrationValue, 4);

  if (newCalibrationValue < 0) {
    Serial.println(F("\n[PERINGATAN] Faktor kalibrasi bernilai MINUS (-)!"));
    Serial.println(F("-> Penyebab: Arah penekanan terbalik dari tanda panah load cell, atau kabel A+ dan A- tertukar."));
  }
  if (abs(rawDelta) < 1000) {
    Serial.println(F("\n[PERINGATAN KRITIS] Perubahan nilai ADC sangat kecil (< 1000 counts)!"));
    Serial.println(F("-> Untuk beban seberat ini, sinyal ADC seharusnya lebih besar."));
    Serial.println(F("-> Pastikan sensor tidak terhambat/menyentuh permukaan selain titik tumpu."));
  }

  // Langkah 4: Konfirmasi penyimpanan ke EEPROM
  Serial.print(F("Apakah ingin menyimpan faktor kalibrasi ke EEPROM (alamat "));
  Serial.print(calVal_eepromAddress);
  Serial.println(F(")? (y/n): "));

  // Bersihkan sisa karakter enter di buffer
  while (Serial.available() > 0) Serial.read();

  _resume = false;
  while (!_resume) {
    if (Serial.available() > 0) {
      char inByte = Serial.read();
      if (inByte == 'y' || inByte == 'Y') {
        EEPROM.put(calVal_eepromAddress, newCalibrationValue);
        float verifiedValue = 0.0;
        EEPROM.get(calVal_eepromAddress, verifiedValue);

        Serial.print(F("-> Berhasil disimpan ke EEPROM: "));
        Serial.println(verifiedValue, 4);
        LoadCell.setCalFactor(newCalibrationValue);
        _resume = true;
      } else if (inByte == 'n' || inByte == 'N') {
        LoadCell.setCalFactor(newCalibrationValue);
        Serial.println(F("-> Nilai digunakan untuk sesi ini saja (TIDAK disimpan ke EEPROM)."));
        _resume = true;
      }
    }
  }

  beratKalman.reset(known_mass);
  displayBerat = known_mass;

  Serial.println(F("************************************************"));
  Serial.println(F("          KALIBRASI SELESAI & BERHASIL          "));
  Serial.println(F("************************************************"));
  printMenu();
}

/**
 * @brief Memasukkan nilai faktor kalibrasi secara manual melalui Serial Monitor.
 */
void changeSavedCalFactor() {
  float currentCalFactor = LoadCell.getCalFactor();
  Serial.println();
  Serial.println(F("------------------------------------------------"));
  Serial.print(F("Faktor kalibrasi saat ini: "));
  Serial.println(currentCalFactor, 4);
  Serial.println(F("Masukkan nilai baru via Serial Monitor (contoh: 1447.0) lalu Enter:"));

  while (Serial.available() > 0) Serial.read();

  float newCalibrationValue = 0.0;
  boolean _resume = false;
  while (!_resume) {
    if (Serial.available() > 0) {
      newCalibrationValue = Serial.parseFloat();
      if (newCalibrationValue != 0.0f) {
        Serial.print(F("-> Nilai faktor kalibrasi baru: "));
        Serial.println(newCalibrationValue, 4);
        LoadCell.setCalFactor(newCalibrationValue);
        _resume = true;
      }
    }
  }

  Serial.print(F("Simpan nilai ini ke EEPROM? (y/n): "));
  while (Serial.available() > 0) Serial.read();

  _resume = false;
  while (!_resume) {
    if (Serial.available() > 0) {
      char inByte = Serial.read();
      if (inByte == 'y' || inByte == 'Y') {
        EEPROM.put(calVal_eepromAddress, newCalibrationValue);
        Serial.println(F("-> Berhasil disimpan ke EEPROM."));
        _resume = true;
      } else if (inByte == 'n' || inByte == 'N') {
        Serial.println(F("-> Nilai tidak disimpan ke EEPROM."));
        _resume = true;
      }
    }
  }
  Serial.println(F("------------------------------------------------"));
  printMenu();
}

/**
 * @brief Menampilkan menu perintah yang tersedia di Serial Monitor.
 */
void printMenu() {
  Serial.println();
  Serial.println(F("--- Petunjuk Perintah Serial Monitor ---"));
  Serial.println(F("  't' : Tare (nol-kan timbangan)"));
  Serial.println(F("  'r' : Kalibrasi ulang timbangan (masukkan beban dalam KG)"));
  Serial.println(F("  'c' : Ubah faktor kalibrasi secara manual"));
  Serial.println(F("  'd' : Toggle info Raw ADC & Delta"));
  Serial.println(F("  '?' : Tampilkan menu petunjuk ini"));
  Serial.println(F("----------------------------------------"));
  Serial.println();
}
