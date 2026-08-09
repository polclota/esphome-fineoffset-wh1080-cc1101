#pragma once

#include "esphome.h"
#include "esphome/components/template/sensor/template_sensor.h"

namespace esphome {
namespace wh1080 {

// Fine Offset WH1080/WH3080 OOK decoder.
// Framing follows rtl_433 fineoffset_wh1080.c:
//   weather/time: 88..99 bits (8-bit preamble) or 87 bits (7-bit preamble)
//   UV/light:     64 bits     (8-bit preamble) or 63 bits (7-bit preamble)
// The CC1101 GDO signal is sampled on CHANGE; only HIGH pulse widths are decoded.

static uint8_t wh1080_pin_ = 19;

static volatile bool wh1080_has_frame = false;
static volatile uint8_t wh1080_last_frame[13] = {0};
static volatile uint8_t wh1080_last_frame_bits = 0;

static volatile uint32_t wh1080_edge_count = 0;
static volatile uint32_t wh1080_valid_pulse_count = 0;
static volatile uint32_t wh1080_frame_count = 0;
static volatile uint32_t wh1080_length_bad_count = 0;
static volatile uint32_t wh1080_reset_count = 0;
static volatile uint32_t wh1080_last_break_pulse = 0;
static volatile uint8_t wh1080_max_bits = 0;

static volatile uint32_t wh1080_hi_hist[5] = {0};
static volatile uint32_t wh1080_lo_hist[5] = {0};

static inline bool wh1080_is_bcd_byte(uint8_t v) {
  return ((v >> 4) & 0x0F) <= 9 && (v & 0x0F) <= 9;
}

static inline bool wh1080_supported_length(uint8_t bits) {
  return (bits >= 88 && bits < 100) || bits == 87 || bits == 64 || bits == 63;
}

static inline uint8_t wh1080_get_bit(const uint8_t *buf, uint16_t bitpos) {
  return (buf[bitpos >> 3] >> (7 - (bitpos & 7))) & 1U;
}

static inline void wh1080_extract_bits(const uint8_t *src, uint16_t start_bit,
                                       uint8_t *dst, uint16_t count_bits) {
  uint16_t bytes = (count_bits + 7) >> 3;
  for (uint16_t i = 0; i < bytes; i++) dst[i] = 0;
  for (uint16_t i = 0; i < count_bits; i++) {
    if (wh1080_get_bit(src, start_bit + i))
      dst[i >> 3] |= (uint8_t) (0x80U >> (i & 7));
  }
}

static void IRAM_ATTR wh1080_isr() {
  wh1080_edge_count++;

  static uint32_t last_time = 0;
  static uint8_t row[13] = {0};
  static uint8_t bit_count = 0;

  uint32_t now = micros();
  uint32_t pulse = now - last_time;
  last_time = now;

  if (pulse < 120) return;

  bool level_now = digitalRead(wh1080_pin_);

  // rtl_433 OOK reset_limit is 2800 us. On the raw GDO stream this is
  // observed as the long LOW gap at the end of a row.
  if (pulse > 2800) {
    if (bit_count > 0) {
      wh1080_frame_count++;
      wh1080_last_break_pulse = pulse;
      if (bit_count > wh1080_max_bits) wh1080_max_bits = bit_count;

      if (wh1080_supported_length(bit_count)) {
        // A second copy 31 ms later may overwrite the first if loop() has not
        // consumed it yet; both copies carry the same payload and this avoids
        // doing any heavy processing inside the ISR.
        for (uint8_t i = 0; i < 13; i++) wh1080_last_frame[i] = row[i];
        wh1080_last_frame_bits = bit_count;
        wh1080_has_frame = true;
      } else {
        wh1080_length_bad_count++;
      }
    }

    bit_count = 0;
    for (uint8_t i = 0; i < 13; i++) row[i] = 0;
    wh1080_reset_count++;
    return;
  }

  // We decode the duration of the HIGH pulse (falling edge => level_now LOW).
  // Keep the threshold that already produced a verified CRC-valid packet on
  // this CC1101/GDO setup. Short pulse = 1, long pulse = 0.
  if (pulse >= 150 && pulse <= 2400) {
    uint8_t bkt = (pulse < 350) ? 0 : (pulse < 550) ? 1 :
                  (pulse < 800) ? 2 : (pulse < 1200) ? 3 : 4;
    if (level_now == LOW) wh1080_hi_hist[bkt]++;
    else                  wh1080_lo_hist[bkt]++;

    if (level_now == LOW) {
      wh1080_valid_pulse_count++;
      bool bit_val = (pulse < 750);

      if (bit_count < 104) {
        uint8_t byte_idx = bit_count >> 3;
        uint8_t bit_idx = bit_count & 7;
        if (bit_val) row[byte_idx] |= (uint8_t) (0x80U >> bit_idx);
        bit_count++;
      }
    }
  }
}

class WH1080Sensor : public Component {
 public:
  sensor::Sensor *temp_sensor = nullptr;
  sensor::Sensor *hum_sensor = nullptr;
  sensor::Sensor *wind_speed_sensor = nullptr;
  sensor::Sensor *wind_gust_sensor = nullptr;
  sensor::Sensor *wind_dir_sensor = nullptr;
  sensor::Sensor *rain_sensor = nullptr;
  sensor::Sensor *station_id_sensor = nullptr;
  sensor::Sensor *uv_sensor = nullptr;
  sensor::Sensor *lux_sensor = nullptr;

  uint32_t last_diag_log_ms_ = 0;
  uint32_t crc_bad_count_ = 0;
  uint32_t header_bad_count_ = 0;
  uint32_t weather_ok_count_ = 0;
  uint32_t datetime_ok_count_ = 0;
  uint32_t uv_ok_count_ = 0;

  WH1080Sensor(uint8_t pin) { wh1080_pin_ = pin; }

  void setup() override {
    pinMode(wh1080_pin_, INPUT);
    attachInterrupt(digitalPinToInterrupt(wh1080_pin_), wh1080_isr, CHANGE);
    ESP_LOGI("wh1080", "ISR enganxada a GPIO%u (CHANGE), framing rtl_433", wh1080_pin_);
  }

  // Same CRC convention used by rtl_433 crc8(..., poly=0x31, init=0xFF):
  // a valid complete row (including CRC byte) has residue 0.
  static uint8_t crc8_rtl433(const uint8_t data[], uint8_t len) {
    uint8_t crc = 0xFF;
    for (uint8_t addr = 0; addr < len; addr++) {
      uint8_t inbyte = data[addr];
      for (uint8_t i = 0; i < 8; i++) {
        uint8_t mix = (crc ^ inbyte) & 0x80;
        crc <<= 1;
        if (mix) crc ^= 0x31;
        inbyte <<= 1;
      }
    }
    return crc;
  }

  static bool normalize_frame(const uint8_t raw[13], uint8_t raw_bits,
                              uint8_t out[11], uint8_t &out_len,
                              uint8_t &preamble_bits) {
    for (uint8_t i = 0; i < 11; i++) out[i] = 0;

    if (raw_bits >= 88 && raw_bits < 100) {
      // rtl_433 EPB path: use first 88 bits directly.
      for (uint8_t i = 0; i < 11; i++) out[i] = raw[i];
      out_len = 11;
      preamble_bits = 8;
    } else if (raw_bits == 87) {
      // rtl_433 SPB path: 7 preamble bits, then extract 80 payload bits.
      out[0] = (uint8_t) ((raw[0] >> 1) | 0x80);
      wh1080_extract_bits(raw, 7, out + 1, 80);
      out_len = 11;
      preamble_bits = 7;
    } else if (raw_bits == 64) {
      // WH3080 UV/light EPB.
      for (uint8_t i = 0; i < 8; i++) out[i] = raw[i];
      out_len = 8;
      preamble_bits = 8;
    } else if (raw_bits == 63) {
      // WH3080 UV/light SPB.
      out[0] = (uint8_t) ((raw[0] >> 1) | 0x80);
      wh1080_extract_bits(raw, 7, out + 1, 56);
      out_len = 8;
      preamble_bits = 7;
    } else {
      return false;
    }

    return true;
  }

  void loop() override {
    uint32_t now_ms = millis();
    if (now_ms - this->last_diag_log_ms_ > 15000) {
      this->last_diag_log_ms_ = now_ms;
      ESP_LOGI("wh1080",
               "diag edges=%lu valid=%lu rows=%lu len_bad=%lu crc_bad=%lu hdr_bad=%lu "
               "ok_w=%lu ok_t=%lu ok_uv=%lu resets=%lu max_bits=%u break=%luus "
               "hi=[%lu,%lu,%lu,%lu,%lu] lo=[%lu,%lu,%lu,%lu,%lu]",
               (unsigned long) wh1080_edge_count,
               (unsigned long) wh1080_valid_pulse_count,
               (unsigned long) wh1080_frame_count,
               (unsigned long) wh1080_length_bad_count,
               (unsigned long) this->crc_bad_count_,
               (unsigned long) this->header_bad_count_,
               (unsigned long) this->weather_ok_count_,
               (unsigned long) this->datetime_ok_count_,
               (unsigned long) this->uv_ok_count_,
               (unsigned long) wh1080_reset_count,
               (unsigned) wh1080_max_bits,
               (unsigned long) wh1080_last_break_pulse,
               (unsigned long) wh1080_hi_hist[0], (unsigned long) wh1080_hi_hist[1],
               (unsigned long) wh1080_hi_hist[2], (unsigned long) wh1080_hi_hist[3],
               (unsigned long) wh1080_hi_hist[4], (unsigned long) wh1080_lo_hist[0],
               (unsigned long) wh1080_lo_hist[1], (unsigned long) wh1080_lo_hist[2],
               (unsigned long) wh1080_lo_hist[3], (unsigned long) wh1080_lo_hist[4]);
    }

    if (!wh1080_has_frame) return;

    uint8_t raw[13];
    uint8_t raw_bits;
    noInterrupts();
    raw_bits = wh1080_last_frame_bits;
    for (uint8_t i = 0; i < 13; i++) raw[i] = wh1080_last_frame[i];
    wh1080_has_frame = false;
    interrupts();

    uint8_t p[11];
    uint8_t packet_len = 0;
    uint8_t preamble_bits = 0;
    if (!normalize_frame(raw, raw_bits, p, packet_len, preamble_bits)) return;

    if (p[0] != 0xFF) {
      this->header_bad_count_++;
      ESP_LOGD("wh1080", "DROP bits=%u pre=%u header=%02X", raw_bits, preamble_bits, p[0]);
      return;
    }

    ESP_LOGD("wh1080",
             "RAW bits=%u pre=%u len=%u: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             raw_bits, preamble_bits, packet_len,
             p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10]);

    if (crc8_rtl433(p, packet_len) != 0) {
      this->crc_bad_count_++;
      return;
    }

    uint8_t msg_type = (p[1] >> 4) & 0x0F;
    if (msg_type != 0x0A && msg_type != 0x0B && msg_type != 0x07) {
      // rtl_433 explicitly rejects 0x03 (WH0530/Alecto WS-1200) and
      // 0x05 (Alecto WS-1200 DCF77) in this decoder.
      this->header_bad_count_++;
      ESP_LOGD("wh1080", "DROP valid CRC but unsupported type=0x%X", msg_type);
      return;
    }

    int station_id = ((p[1] << 4) & 0xF0) | ((p[2] >> 4) & 0x0F);
    if (station_id_sensor) station_id_sensor->publish_state(station_id);

    if (msg_type == 0x0A) {
      if (packet_len != 11) {
        this->header_bad_count_++;
        return;
      }

      int temp_raw = ((p[2] & 0x03) << 8) | p[3];
      float temp = (temp_raw - 400) * 0.1f;
      int hum = p[4];
      float speed = (p[5] * 0.34f) * 3.6f;
      float gust = (p[6] * 0.34f) * 3.6f;
      int rain_raw = ((p[7] & 0x0F) << 8) | p[8];
      float rain = rain_raw * 0.3f;
      static const int wind_dir_degr[] = {0, 23, 45, 68, 90, 113, 135, 158,
                                          180, 203, 225, 248, 270, 293, 315, 338};
      int dir = wind_dir_degr[p[9] & 0x0F];

      // Extra sanity only after CRC, so it cannot hide framing/CRC diagnostics.
      if (temp < -45.0f || temp > 70.0f || hum > 100 || speed > 180.0f ||
          gust > 220.0f || rain > 5000.0f) {
        this->header_bad_count_++;
        return;
      }

      this->weather_ok_count_++;
      if (temp_sensor && temp_sensor->state != temp) temp_sensor->publish_state(temp);
      if (hum_sensor && hum_sensor->state != hum) hum_sensor->publish_state(hum);
      if (wind_speed_sensor && wind_speed_sensor->state != speed) wind_speed_sensor->publish_state(speed);
      if (wind_gust_sensor && wind_gust_sensor->state != gust) wind_gust_sensor->publish_state(gust);
      if (rain_sensor && rain_sensor->state != rain) rain_sensor->publish_state(rain);
      if (wind_dir_sensor && wind_dir_sensor->state != dir) wind_dir_sensor->publish_state(dir);

      ESP_LOGI("wh1080",
               "OK weather bits=%u pre=%u id=%d Temp=%.1f Hum=%d Spd=%.1f Gust=%.1f Rain=%.1f Dir=%d",
               raw_bits, preamble_bits, station_id, temp, hum, speed, gust, rain, dir);

    } else if (msg_type == 0x0B) {
      if (packet_len != 11) {
        this->header_bad_count_++;
        return;
      }

      if (!wh1080_is_bcd_byte(p[3]) || !wh1080_is_bcd_byte(p[4]) ||
          !wh1080_is_bcd_byte(p[5]) || !wh1080_is_bcd_byte(p[6]) ||
          !wh1080_is_bcd_byte(p[8])) {
        this->header_bad_count_++;
        return;
      }

      int hh = ((p[3] & 0x30) >> 4) * 10 + (p[3] & 0x0F);
      int mm = ((p[4] & 0xF0) >> 4) * 10 + (p[4] & 0x0F);
      int ss = ((p[5] & 0xF0) >> 4) * 10 + (p[5] & 0x0F);
      int year = ((p[6] & 0xF0) >> 4) * 10 + (p[6] & 0x0F) + 2000;
      int month = ((p[7] & 0x10) >> 4) * 10 + (p[7] & 0x0F);
      int day = ((p[8] & 0xF0) >> 4) * 10 + (p[8] & 0x0F);

      if (hh > 23 || mm > 59 || ss > 59 || month < 1 || month > 12 || day < 1 || day > 31) {
        this->header_bad_count_++;
        return;
      }

      this->datetime_ok_count_++;
      ESP_LOGI("wh1080", "OK datetime bits=%u pre=%u id=%d %04d-%02d-%02d %02d:%02d:%02d",
               raw_bits, preamble_bits, station_id, year, month, day, hh, mm, ss);

    } else {  // 0x07 WH3080 UV/light
      if (packet_len != 8) {
        this->header_bad_count_++;
        return;
      }

      int uv_index = p[2] & 0x0F;
      bool uv_status_ok = (p[3] == 0x55);
      int light_raw = (p[4] << 16) | (p[5] << 8) | p[6];
      float lux = light_raw * 0.1f;

      this->uv_ok_count_++;
      if (uv_sensor && uv_sensor->state != uv_index) uv_sensor->publish_state(uv_index);
      if (lux_sensor && lux_sensor->state != lux) lux_sensor->publish_state(lux);

      ESP_LOGI("wh1080", "OK uv/light bits=%u pre=%u id=%d status=%s UV=%d Lux=%.1f",
               raw_bits, preamble_bits, station_id, uv_status_ok ? "OK" : "ERROR", uv_index, lux);
    }
  }
};

static WH1080Sensor *wh1080_instance = nullptr;

static WH1080Sensor *create_instance(uint8_t pin) {
  if (wh1080_instance == nullptr) wh1080_instance = new WH1080Sensor(pin);
  return wh1080_instance;
}

static WH1080Sensor *get_instance() { return wh1080_instance; }

}  // namespace wh1080
}  // namespace esphome