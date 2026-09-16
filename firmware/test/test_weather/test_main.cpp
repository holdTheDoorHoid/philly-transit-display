#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "weather_core/weather.h"

using namespace weather;

namespace {
std::vector<uint8_t> readFixture(const char* name) {
  std::string path = std::string("test/fixtures/") + name;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    std::string here = __FILE__;  // .../firmware/test/test_weather/test_main.cpp
    here.erase(here.find_last_of("/\\"));
    here.erase(here.find_last_of("/\\"));
    path = here + "/fixtures/" + name;
    f = std::fopen(path.c_str(), "rb");
  }
  TEST_ASSERT_NOT_NULL_MESSAGE(f, "fixture not found");
  std::vector<uint8_t> out;
  uint8_t buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return out;
}
}  // namespace

void setUp(void) {}
void tearDown(void) {}

// Captured 2026-09-14 09:45 EDT for 39.928,-75.177 (19th & Mifflin).
void test_parse_open_meteo_fixture() {
  std::vector<uint8_t> body = readFixture("openmeteo_19th_mifflin.json");
  Forecast f;
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(parseOpenMeteo(body.data(), body.size(), &f, &err), err.c_str());
  TEST_ASSERT_TRUE(f.valid());
  TEST_ASSERT_EQUAL_INT(1, f.code);
  TEST_ASSERT_EQUAL_STRING("mostly clear", codeText(f.code));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 69.4f, f.temp);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 68.7f, f.feels_like);
  TEST_ASSERT_EQUAL_INT32(-14400, f.utc_offset_s);
  TEST_ASSERT_EQUAL_INT64(1789393500, f.observed);  // 2026-09-14 09:45 EDT
  TEST_ASSERT_EQUAL_UINT32(6, static_cast<uint32_t>(f.hours.size()));
  TEST_ASSERT_EQUAL_INT64(1789390800, f.hours[0].epoch);  // 09:00 EDT
  TEST_ASSERT_EQUAL_INT(3, f.hours[0].code);
  TEST_ASSERT_EQUAL_INT(0, f.hours[0].precip_prob);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 73.5f, f.hours[5].temp);
  // at(): 10:15 EDT falls in the 10:00 slot; 16:00 is past the six-hour horizon.
  const Hour* h = f.at(1789395300);
  TEST_ASSERT_NOT_NULL(h);
  TEST_ASSERT_EQUAL_INT64(1789394400, h->epoch);
  TEST_ASSERT_NULL(f.at(1789390800 + 6 * 3600));
}

void test_parse_open_meteo_error_shape() {
  const char* body = "{\"error\":true,\"reason\":\"Latitude must be in range\"}";
  Forecast f;
  std::string err;
  TEST_ASSERT_FALSE(parseOpenMeteo(reinterpret_cast<const uint8_t*>(body), std::strlen(body), &f, &err));
  TEST_ASSERT_EQUAL_STRING("Latitude must be in range", err.c_str());
  TEST_ASSERT_FALSE(f.valid());
}

void test_iso_local_parse() {
  TEST_ASSERT_EQUAL_INT64(1789393500, parseIsoLocal("2026-09-14T09:45", -14400));
  TEST_ASSERT_EQUAL_INT64(0, parseIsoLocal("garbage", 0));
  TEST_ASSERT_EQUAL_INT64(0, parseIsoLocal("2026-13-14T09:45", 0));
}

void test_code_vocabulary() {
  TEST_ASSERT_TRUE(kindForCode(0) == Kind::Clear);
  TEST_ASSERT_TRUE(kindForCode(3) == Kind::Cloudy);
  TEST_ASSERT_TRUE(kindForCode(48) == Kind::Fog);
  TEST_ASSERT_TRUE(kindForCode(63) == Kind::Rain);
  TEST_ASSERT_TRUE(kindForCode(67) == Kind::FreezingRain);
  TEST_ASSERT_TRUE(kindForCode(75) == Kind::Snow);
  TEST_ASSERT_TRUE(kindForCode(81) == Kind::Showers);
  TEST_ASSERT_TRUE(kindForCode(96) == Kind::Thunder);
  TEST_ASSERT_TRUE(kindForCode(42) == Kind::Unknown);
  TEST_ASSERT_TRUE(isWet(Kind::Rain));
  TEST_ASSERT_FALSE(isWet(Kind::Fog));
  TEST_ASSERT_EQUAL_STRING("thunderstorm", codeText(95));
  TEST_ASSERT_EQUAL_STRING("", codeText(42));
}

void test_notable_rules() {
  Hour rain;
  rain.code = 61;
  rain.precip_prob = 20;
  TEST_ASSERT_TRUE(notable(rain, 0));   // wet is always worth a line
  Hour likely;
  likely.code = 2;
  likely.precip_prob = 55;
  TEST_ASSERT_TRUE(notable(likely, 2));  // dry now but rain likely
  Hour cloudy;
  cloudy.code = 3;
  cloudy.precip_prob = 5;
  TEST_ASSERT_FALSE(notable(cloudy, 0));  // clear vs overcast: not worth it
  Hour fog;
  fog.code = 45;
  fog.precip_prob = 0;
  TEST_ASSERT_TRUE(notable(fog, 1));
  TEST_ASSERT_FALSE(notable(fog, 48));    // same kind as the header already shows
  Hour none;
  TEST_ASSERT_FALSE(notable(none, 0));
}

void test_url_builder() {
  std::string url = openMeteoUrl(39.927947, -75.177147, true, 6);
  TEST_ASSERT_EQUAL_STRING(
      "https://api.open-meteo.com/v1/forecast?latitude=39.9279&longitude=-75.1771"
      "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m"
      "&hourly=weather_code,precipitation_probability,temperature_2m"
      "&forecast_hours=6&timezone=auto&wind_speed_unit=mph&temperature_unit=fahrenheit",
      url.c_str());
  TEST_ASSERT_TRUE(openMeteoUrl(0, 0, false, 99).find("forecast_hours=24") != std::string::npos);
}

void test_nearby() {
  // 19th & Mifflin vs 20th & Mifflin: ~130 m apart.
  TEST_ASSERT_TRUE(nearby(39.927947, -75.177147, 39.927942, -75.178646, 1.5));
  // 19th & Mifflin vs 30th Street Station: ~3 km.
  TEST_ASSERT_FALSE(nearby(39.927947, -75.177147, 39.9557, -75.1820, 1.5));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parse_open_meteo_fixture);
  RUN_TEST(test_parse_open_meteo_error_shape);
  RUN_TEST(test_iso_local_parse);
  RUN_TEST(test_code_vocabulary);
  RUN_TEST(test_notable_rules);
  RUN_TEST(test_url_builder);
  RUN_TEST(test_nearby);
  return UNITY_END();
}
