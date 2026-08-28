#ifndef KALMAN_FILTER_H
#define KALMAN_FILTER_H

#include <Arduino.h>

/**
 * @brief Adaptive 1D Kalman Filter untuk Load Cell HX711.
 * - Saat diam: R (measurement noise) disesuaikan dengan variansi sensor (~100.000 - 200.000),
 *   sehingga fluktuasi noise ±1000 diredam menjadi sangat stabil (hanya bergerak ±1-2 digit).
 * - Saat ada beban nyata (selisih > threshold): Filter menaikkan gain secara instan agar melompat
 *   ke nilai beban baru dalam 1-2 siklus tanpa lag.
 */
class KalmanFilter {
private:
    float _q;         // Process noise dasar
    float _r;         // Measurement noise (disesuaikan dengan skala ADC 24-bit HX711)
    float _p;         // Estimation error covariance
    float _x;         // State estimate (nilai terfilter)
    float _k;         // Kalman gain
    float _threshold; // Ambang batas beban nyata (harus di atas noise floor sensor)

public:
    /**
     * @param q Process noise dasar (default: 15.0f)
     * @param r Measurement noise (default: 150000.0f untuk menekan noise ADC ratusan/ribuan digit)
     * @param threshold Batas deteksi perubahan beban nyata (default: 1200.0f)
     * @param p Covariance awal
     * @param initial_value Nilai awal
     */
    KalmanFilter(float q = 15.0f, float r = 150000.0f, float threshold = 1200.0f, float p = 1000.0f, float initial_value = 0.0f)
        : _q(q), _r(r), _p(p), _x(initial_value), _k(0.0f), _threshold(threshold) {}

    /**
     * @brief Update filter dengan nilai pengukuran baru
     * @param measurement Nilai bacaan mentah dari sensor
     * @return float Nilai yang telah diestimasi/difilter
     */
    float update(float measurement) {
        float diff = fabs(measurement - _x);
        float dynamic_q = _q;

        // ADAPTIVE LOGIC:
        // Jika selisih melebihi threshold noise (beban nyata berubah),
        // naikkan Q secara drastis proporsional terhadap besar beban.
        if (diff > _threshold) {
            float excess = diff - _threshold;
            dynamic_q = _q + excess * 50.0f;
        }

        // 1. Prediction update
        _p = _p + dynamic_q;

        // 2. Measurement update
        _k = _p / (_p + _r);
        _x = _x + _k * (measurement - _x);
        _p = (1.0f - _k) * _p;

        return _x;
    }

    /**
     * @brief Mengatur parameter filter
     */
    void setParameters(float q, float r, float threshold) {
        _q = q;
        _r = r;
        _threshold = threshold;
    }

    /**
     * @brief Reset nilai filter
     */
    void reset(float initial_value = 0.0f, float initial_p = 1000.0f) {
        _x = initial_value;
        _p = initial_p;
        _k = 0.0f;
    }

    /**
     * @brief Mengambil nilai estimasi terakhir
     */
    float getState() const {
        return _x;
    }
};

#endif // KALMAN_FILTER_H
