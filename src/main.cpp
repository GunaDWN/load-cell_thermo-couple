#include <Arduino.h>
#include <HX711_ADC.h>
#include <EEPROM.h>
#include <max6675.h>
#include "KalmanFilter.h"

// ============================================================================
// KONFIGURASI PIN HARDWARE ARDUINO NANO
// ============================================================================
// Load Cell 1 (HX711 Modul 1)
const int HX711_1_DOUT_PIN = 4; // Pin D4 (Data / DOUT)
const int HX711_1_SCK_PIN  = 5; // Pin D5 (Clock / SCK)

// Load Cell 2 (HX711 Modul 2)
const int HX711_2_DOUT_PIN = 2; // Pin D2 (Data / DOUT)
const int HX711_2_SCK_PIN  = 3; // Pin D3 (Clock / SCK)

// Termokopel 1 (MAX6675 Modul 1)
const int thermo1_CLK = 6; // Pin D6 (SCK / Clock)
const int thermo1_CS  = 7; // Pin D7 (CS / Chip Select)
const int thermo1_DO  = 8; // Pin D8 (SO / Data Out)

// Termokopel 2 (MAX6675 Modul 2)
const int thermo2_CS  = 9;  // Pin D9 (CS / Chip Select)
const int thermo2_CLK = 10; // Pin D10 (SCK / Clock)
const int thermo2_DO  = 11; // Pin D11 (SO / Data Out)

// ============================================================================
// INSTANSIASI OBJEK SENSOR
// ============================================================================
HX711_ADC LoadCell1(HX711_1_DOUT_PIN, HX711_1_SCK_PIN);
HX711_ADC LoadCell2(HX711_2_DOUT_PIN, HX711_2_SCK_PIN);

MAX6675 thermocouple1(thermo1_CLK, thermo1_CS, thermo1_DO);
MAX6675 thermocouple2(thermo2_CLK, thermo2_CS, thermo2_DO);

// ============================================================================
// FILTER KALMAN ADAPTIF
// ============================================================================
// Filter untuk Load Cell (satuan kg)
KalmanFilter beratKalman1(0.0001f, 0.20f, 0.08f);
KalmanFilter beratKalman2(0.0001f, 0.20f, 0.08f);

// Filter untuk Suhu MAX6675 (satuan Celcius)
KalmanFilter suhuKalman1(0.02f, 1.50f, 2.0f);
KalmanFilter suhuKalman2(0.02f, 1.50f, 2.0f);

// ============================================================================
// VARIABEL KONDISI & DISPLAY
// ============================================================================
// Nilai terfilter untuk display
float displayBerat1 = 0.0f;
float displayBerat2 = 0.0f;
float displayTempC1 = 0.0f;
float displayTempF1 = 0.0f;
float displayTempC2 = 0.0f;
float displayTempF2 = 0.0f;

bool suhu1Initialized = false;
bool suhu2Initialized = false;

// Alamat EEPROM untuk kalibrasi (float = 4 byte)
const int calVal_eepromAddress1 = 0; // Byte 0..3
const int calVal_eepromAddress2 = 4; // Byte 4..7

// Variabel Waktu
unsigned long prevPrintTime = 0;
const unsigned long printInterval = 500; // Kirim data JSON setiap 500 ms (2x per detik)

// Konstanta percepatan gravitasi bumi (m/s^2)
const float GRAVITY = 9.80665f;

// Automatic Zero Tracking (AZT) & Stable Lock Parameters
const float AZT_WINDOW = 0.120f;       // Drift < 120 gram dianggap hanyutan nol saat kosong
const float STABLE_TOLERANCE = 0.040f; // Toleransi stabil 40 gram
unsigned long lastChangeTime1 = 0;
unsigned long lastChangeTime2 = 0;
bool isWeightLocked1 = false;
bool isWeightLocked2 = false;

// Format JSON: true = Compact (1 baris per paket - Standar Komunikasi Serial IoT)
bool compactJSON = true;

// Deklarasi fungsi
void calibrateLoadCell(uint8_t cellNum, HX711_ADC &cell, int eepromAddr, KalmanFilter &kalman, float &disp, unsigned long &lastTime, bool &isLocked);
void printJSON();
void printMenu();

void setup() {
  Serial.begin(57600);
  delay(10);
  Serial.println();
  Serial.println(F("=========================================================="));
  Serial.println(F(" Sistem Monitoring 2x Load Cell & 2x Termokopel MAX6675 "));
  Serial.println(F("=========================================================="));

  // Menunggu stabilisasi modul termokopel MAX6675
  delay(500);

  // Inisialisasi Load Cell
  LoadCell1.begin();
  LoadCell2.begin();

  // Polaritas: Ditekan (compression) bernilai Negatif (-), Direntangkan (tension) bernilai Positif (+)
  // setReverseOutput() dinonaktifkan agar sesuai polaritas fisik standar
  // LoadCell1.setReverseOutput();
  // LoadCell2.setReverseOutput();

  // Baca faktor kalibrasi dari EEPROM
  float calValue1 = 0.0f;
  float calValue2 = 0.0f;
  EEPROM.get(calVal_eepromAddress1, calValue1);
  EEPROM.get(calVal_eepromAddress2, calValue2);

  if (isnan(calValue1) || isinf(calValue1) || calValue1 == 0.0f || calValue1 == 1.0f) {
    calValue1 = 1412.86f;
    Serial.println(F("[INFO] Load Cell 1 menggunakan faktor kalibrasi awal 1412.86."));
  } else {
    calValue1 = fabs(calValue1);
    Serial.print(F("[INFO] Load Cell 1 Cal Factor: "));
    Serial.println(calValue1, 2);
  }

  if (isnan(calValue2) || isinf(calValue2) || calValue2 == 0.0f || calValue2 == 1.0f) {
    calValue2 = 1412.86f;
    Serial.println(F("[INFO] Load Cell 2 menggunakan faktor kalibrasi awal 1412.86."));
  } else {
    calValue2 = fabs(calValue2);
    Serial.print(F("[INFO] Load Cell 2 Cal Factor: "));
    Serial.println(calValue2, 2);
  }

  unsigned long stabilizingtime = 2000; // Stabilisasi 2 detik saat tare awal
  boolean _tare = true;
  LoadCell1.start(stabilizingtime, _tare);
  LoadCell2.start(stabilizingtime, _tare);

  LoadCell1.setCalFactor(calValue1);
  LoadCell2.setCalFactor(calValue2);

  lastChangeTime1 = millis();
  lastChangeTime2 = millis();

  Serial.println(F("[STATUS] Inisialisasi selesai. Sensor siap digunakan!"));
  printMenu();
}

void loop() {
  // Update konversi HX711 (non-blocking)
  LoadCell1.update();
  LoadCell2.update();

  // Kirim output JSON secara berkala
  if (millis() - prevPrintTime >= printInterval) {
    // -------------------------------------------------------------
    // PEMROSESAN LOAD CELL 1
    // -------------------------------------------------------------
    float rawBerat1 = LoadCell1.getData();
    if (isnan(rawBerat1) || isinf(rawBerat1)) {
      rawBerat1 = 0.0f;
    }
    if (rawBerat1 < -500.0f) rawBerat1 = -500.0f;
    if (rawBerat1 > 500.0f) rawBerat1 = 500.0f;

    float filtered1 = beratKalman1.update(rawBerat1);

    if (fabs(filtered1) < AZT_WINDOW) {
      // Automatic Zero Tracking saat kosong
      long curTare1 = LoadCell1.getTareOffset();
      float cal1 = LoadCell1.getCalFactor();
      LoadCell1.setTareOffset(curTare1 + (long)(filtered1 * cal1 * 0.10f));
      displayBerat1 = 0.0f;
      isWeightLocked1 = false;
      lastChangeTime1 = millis();
    } else {
      if (fabs(filtered1 - displayBerat1) > STABLE_TOLERANCE) {
        isWeightLocked1 = false;
        displayBerat1 = filtered1;
        lastChangeTime1 = millis();
      } else {
        if (!isWeightLocked1 && (millis() - lastChangeTime1 >= 1200)) {
          isWeightLocked1 = true;
          displayBerat1 = filtered1;
        }
        if (!isWeightLocked1) {
          displayBerat1 = filtered1;
        }
      }
    }

    // -------------------------------------------------------------
    // PEMROSESAN LOAD CELL 2
    // -------------------------------------------------------------
    float rawBerat2 = LoadCell2.getData();
    if (isnan(rawBerat2) || isinf(rawBerat2)) {
      rawBerat2 = 0.0f;
    }
    if (rawBerat2 < -500.0f) rawBerat2 = -500.0f;
    if (rawBerat2 > 500.0f) rawBerat2 = 500.0f;

    float filtered2 = beratKalman2.update(rawBerat2);

    if (fabs(filtered2) < AZT_WINDOW) {
      // Automatic Zero Tracking saat kosong
      long curTare2 = LoadCell2.getTareOffset();
      float cal2 = LoadCell2.getCalFactor();
      LoadCell2.setTareOffset(curTare2 + (long)(filtered2 * cal2 * 0.10f));
      displayBerat2 = 0.0f;
      isWeightLocked2 = false;
      lastChangeTime2 = millis();
    } else {
      if (fabs(filtered2 - displayBerat2) > STABLE_TOLERANCE) {
        isWeightLocked2 = false;
        displayBerat2 = filtered2;
        lastChangeTime2 = millis();
      } else {
        if (!isWeightLocked2 && (millis() - lastChangeTime2 >= 1200)) {
          isWeightLocked2 = true;
          displayBerat2 = filtered2;
        }
        if (!isWeightLocked2) {
          displayBerat2 = filtered2;
        }
      }
    }

    // -------------------------------------------------------------
    // PEMROSESAN TERMOKOPEL 1
    // -------------------------------------------------------------
    float rawTempC1 = thermocouple1.readCelsius();
    if (!isnan(rawTempC1) && rawTempC1 >= -10.0f && rawTempC1 <= 800.0f) {
      if (!suhu1Initialized) {
        suhuKalman1.reset(rawTempC1);
        displayTempC1 = rawTempC1;
        suhu1Initialized = true;
      }
      float filteredTempC1 = suhuKalman1.update(rawTempC1);
      if (fabs(filteredTempC1 - displayTempC1) >= 0.20f) {
        displayTempC1 = filteredTempC1;
      }
      displayTempF1 = displayTempC1 * 1.8f + 32.0f;
    }

    // -------------------------------------------------------------
    // PEMROSESAN TERMOKOPEL 2
    // -------------------------------------------------------------
    float rawTempC2 = thermocouple2.readCelsius();
    if (!isnan(rawTempC2) && rawTempC2 >= -10.0f && rawTempC2 <= 800.0f) {
      if (!suhu2Initialized) {
        suhuKalman2.reset(rawTempC2);
        displayTempC2 = rawTempC2;
        suhu2Initialized = true;
      }
      float filteredTempC2 = suhuKalman2.update(rawTempC2);
      if (fabs(filteredTempC2 - displayTempC2) >= 0.20f) {
        displayTempC2 = filteredTempC2;
      }
      displayTempF2 = displayTempC2 * 1.8f + 32.0f;
    }

    // Cetak JSON ke Serial Monitor
    printJSON();

    prevPrintTime = millis();
  }

  // ---------------------------------------------------------------
  // PENANGANAN PERINTAH SERIAL MONITOR
  // ---------------------------------------------------------------
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() > 0) {
      if (cmd.equals("t")) {
        Serial.println(F("-> Melakukan Tare (set titik nol) kedua Load Cell..."));
        LoadCell1.tareNoDelay();
        LoadCell2.tareNoDelay();
      } else if (cmd.equals("CAL1_ZERO")) {
        LoadCell1.tareNoDelay();
        Serial.println(F("[CAL_STATUS] ZERO1_START"));
      } else if (cmd.equals("CAL2_ZERO")) {
        LoadCell2.tareNoDelay();
        Serial.println(F("[CAL_STATUS] ZERO2_START"));
      } else if (cmd.startsWith("CAL1_LOAD:")) {
        float mass = cmd.substring(10).toFloat();
        if (mass > 0.0001f) {
          unsigned long startSettle = millis();
          while (millis() - startSettle < 1500) {
            LoadCell1.update();
            delay(5);
          }
          LoadCell1.refreshDataSet();
          float newCal = fabs(LoadCell1.getNewCalibration(mass));
          if (!isnan(newCal) && !isinf(newCal) && newCal > 0.001f) {
            EEPROM.put(calVal_eepromAddress1, newCal);
            LoadCell1.setCalFactor(newCal);
            float curReading1 = LoadCell1.getData();
            beratKalman1.reset(curReading1);
            displayBerat1 = curReading1;
            isWeightLocked1 = false;
            lastChangeTime1 = millis();
            Serial.print(F("[CAL_STATUS] CAL1_SUCCESS:"));
            Serial.println(newCal, 4);
          }
        }
      } else if (cmd.startsWith("CAL2_LOAD:")) {
        float mass = cmd.substring(10).toFloat();
        if (mass > 0.0001f) {
          unsigned long startSettle = millis();
          while (millis() - startSettle < 1500) {
            LoadCell2.update();
            delay(5);
          }
          LoadCell2.refreshDataSet();
          float newCal = fabs(LoadCell2.getNewCalibration(mass));
          if (!isnan(newCal) && !isinf(newCal) && newCal > 0.001f) {
            EEPROM.put(calVal_eepromAddress2, newCal);
            LoadCell2.setCalFactor(newCal);
            float curReading2 = LoadCell2.getData();
            beratKalman2.reset(curReading2);
            displayBerat2 = curReading2;
            isWeightLocked2 = false;
            lastChangeTime2 = millis();
            Serial.print(F("[CAL_STATUS] CAL2_SUCCESS:"));
            Serial.println(newCal, 4);
          }
        }
      } else if (cmd.equals("1")) {
        calibrateLoadCell(1, LoadCell1, calVal_eepromAddress1, beratKalman1, displayBerat1, lastChangeTime1, isWeightLocked1);
      } else if (cmd.equals("2")) {
        calibrateLoadCell(2, LoadCell2, calVal_eepromAddress2, beratKalman2, displayBerat2, lastChangeTime2, isWeightLocked2);
      } else if (cmd.equals("j")) {
        compactJSON = !compactJSON;
        Serial.print(F("-> Format JSON diubah: "));
        Serial.println(compactJSON ? F("COMPACT (1 baris)") : F("INDENTED (Multi-baris)"));
      } else if (cmd.equals("?") || cmd.equals("h")) {
        printMenu();
      }
    }
  }

  // Cek status Tare Load Cell 1
  if (LoadCell1.getTareStatus() == true) {
    beratKalman1.reset(0.0f);
    displayBerat1 = 0.0f;
    isWeightLocked1 = false;
    lastChangeTime1 = millis();
    Serial.println(F("[CAL_STATUS] ZERO1_DONE"));
    Serial.println(F("[STATUS] Tare Load Cell 1 selesai."));
  }

  // Cek status Tare Load Cell 2
  if (LoadCell2.getTareStatus() == true) {
    beratKalman2.reset(0.0f);
    displayBerat2 = 0.0f;
    isWeightLocked2 = false;
    lastChangeTime2 = millis();
    Serial.println(F("[CAL_STATUS] ZERO2_DONE"));
    Serial.println(F("[STATUS] Tare Load Cell 2 selesai."));
  }
}

/**
 * @brief Menampilkan data dalam format JSON sesuai spesifikasi
 */
void printJSON() {
  float gaya1 = displayBerat1 * GRAVITY;
  float gaya2 = displayBerat2 * GRAVITY;

  float rawTempC1 = thermocouple1.readCelsius();
  float rawTempC2 = thermocouple2.readCelsius();

  if (compactJSON) {
    // Mode satu baris (Compact)
    Serial.print(F("{\"loadcell1\":{\"berat\":"));
    Serial.print(displayBerat1, 2);
    Serial.print(F(",\"gaya\":"));
    Serial.print(gaya1, 1);
    Serial.print(F("},\"loadcell2\":{\"berat\":"));
    Serial.print(displayBerat2, 2);
    Serial.print(F(",\"gaya\":"));
    Serial.print(gaya2, 1);
    Serial.print(F("},\"termokopel1\":{\"suhu\":"));
    if (isnan(rawTempC1)) Serial.print(F("null")); else Serial.print(displayTempC1, 1);
    Serial.print(F(",\"fahrenheit\":"));
    if (isnan(rawTempC1)) Serial.print(F("null")); else Serial.print(displayTempF1, 1);
    Serial.print(F("},\"termokopel2\":{\"suhu\":"));
    if (isnan(rawTempC2)) Serial.print(F("null")); else Serial.print(displayTempC2, 1);
    Serial.print(F(",\"fahrenheit\":"));
    if (isnan(rawTempC2)) Serial.print(F("null")); else Serial.print(displayTempF2, 1);
    Serial.println(F("}}"));
  } else {
    // Mode berlekuk (Indented)
    Serial.println(F("{"));
    Serial.println(F("  \"loadcell1\": {"));
    Serial.print(F("    \"berat\": ")); Serial.print(displayBerat1, 2); Serial.println(F(","));
    Serial.print(F("    \"gaya\": "));  Serial.print(gaya1, 1); Serial.println();
    Serial.println(F("  },"));
    Serial.println(F("  \"loadcell2\": {"));
    Serial.print(F("    \"berat\": ")); Serial.print(displayBerat2, 2); Serial.println(F(","));
    Serial.print(F("    \"gaya\": "));  Serial.print(gaya2, 1); Serial.println();
    Serial.println(F("  },"));
    Serial.println(F("  \"termokopel1\": {"));
    Serial.print(F("    \"suhu\": "));
    if (isnan(rawTempC1)) Serial.print(F("null")); else Serial.print(displayTempC1, 1);
    Serial.println(F(","));
    Serial.print(F("    \"fahrenheit\": "));
    if (isnan(rawTempC1)) Serial.print(F("null")); else Serial.print(displayTempF1, 1);
    Serial.println();
    Serial.println(F("  },"));
    Serial.println(F("  \"termokopel2\": {"));
    Serial.print(F("    \"suhu\": "));
    if (isnan(rawTempC2)) Serial.print(F("null")); else Serial.print(displayTempC2, 1);
    Serial.println(F(","));
    Serial.print(F("    \"fahrenheit\": "));
    if (isnan(rawTempC2)) Serial.print(F("null")); else Serial.print(displayTempF2, 1);
    Serial.println();
    Serial.println(F("  }"));
    Serial.println(F("}"));
  }
}

/**
 * @brief Prosedur kalibrasi interaktif untuk Load Cell tertentu
 */
void calibrateLoadCell(uint8_t cellNum, HX711_ADC &cell, int eepromAddr, KalmanFilter &kalman, float &disp, unsigned long &lastTime, bool &isLocked) {
  Serial.println();
  Serial.println(F("************************************************"));
  Serial.print(F("        KALIBRASI LOAD CELL "));
  Serial.println(cellNum);
  Serial.println(F("************************************************"));
  Serial.println(F("1. Pastikan TIDAK ADA BEBAN di atas sensor."));
  Serial.println(F("2. Kirim karakter 't' untuk Tare titik nol..."));

  while (Serial.available() > 0) Serial.read();

  boolean _resume = false;
  while (!_resume) {
    cell.update();
    if (Serial.available() > 0) {
      char inByte = Serial.read();
      if (inByte == 't') cell.tareNoDelay();
    }
    if (cell.getTareStatus() == true) {
      Serial.println(F("-> Titik nol (Tare) berhasil disetel!"));
      _resume = true;
    }
  }

  Serial.println();
  Serial.println(F("3. Letakkan BEBAN YANG SUDAH DIKETAHUI bobotnya di atas sensor."));
  Serial.println(F("4. Masukkan bobotnya dalam KG (contoh: 1.446 atau 50.0) lalu tekan Enter:"));

  float known_mass = 0.0f;
  _resume = false;
  while (!_resume) {
    cell.update();
    if (Serial.available() > 0) {
      known_mass = Serial.parseFloat();
      if (known_mass != 0.0f) {
        Serial.print(F("-> Bobot referensi diterima: "));
        Serial.print(known_mass, 3);
        Serial.println(F(" kg"));
        _resume = true;
      }
    }
  }

  Serial.println(F("-> Menunggu stabilisasi mekanik 2 detik..."));
  unsigned long settleStart = millis();
  while (millis() - settleStart < 2000) {
    cell.update();
  }

  cell.refreshDataSet();
  float newCalValue = fabs(cell.getNewCalibration(known_mass));

  long tareOffset = cell.getTareOffset();
  long rawWithLoad = (long)(cell.getData() * newCalValue) + tareOffset;
  long rawDelta = rawWithLoad - tareOffset;

  Serial.println();
  Serial.print(F("-> Tare Offset (Kosong) : ")); Serial.println(tareOffset);
  Serial.print(F("-> Raw dengan Beban     : ")); Serial.println(rawWithLoad);
  Serial.print(F("-> Delta Raw ADC        : ")); Serial.println(rawDelta);
  Serial.print(F("-> Faktor Kalibrasi Baru: ")); Serial.println(newCalValue, 4);

  Serial.print(F("Simpan ke EEPROM (alamat "));
  Serial.print(eepromAddr);
  Serial.println(F(")? (y/n): "));

  while (Serial.available() > 0) Serial.read();

  _resume = false;
  while (!_resume) {
    if (Serial.available() > 0) {
      char inByte = Serial.read();
      if (inByte == 'y' || inByte == 'Y') {
        EEPROM.put(eepromAddr, newCalValue);
        Serial.println(F("-> Berhasil disimpan ke EEPROM."));
        cell.setCalFactor(newCalValue);
        _resume = true;
      } else if (inByte == 'n' || inByte == 'N') {
        cell.setCalFactor(newCalValue);
        Serial.println(F("-> Digunakan untuk sesi ini saja (TIDAK disimpan)."));
        _resume = true;
      }
    }
  }

  float curD = cell.getData();
  kalman.reset(curD);
  disp = curD;
  isLocked = true;
  lastTime = millis();

  Serial.println(F("************************************************"));
  Serial.println(F("              KALIBRASI SELESAI                 "));
  Serial.println(F("************************************************"));
  printMenu();
}

/**
 * @brief Menampilkan menu panduan perintah serial
 */
void printMenu() {
  Serial.println();
  Serial.println(F("--- Menu Perintah Serial Monitor ---"));
  Serial.println(F("  't' : Tare (nol-kan kedua load cell)"));
  Serial.println(F("  '1' : Kalibrasi Load Cell 1"));
  Serial.println(F("  '2' : Kalibrasi Load Cell 2"));
  Serial.println(F("  'j' : Toggle format JSON (Indented / Compact)"));
  Serial.println(F("  '?' : Tampilkan menu ini"));
  Serial.println(F("------------------------------------"));
  Serial.println();
}
