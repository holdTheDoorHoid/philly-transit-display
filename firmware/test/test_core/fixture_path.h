// Test-only helper: resolves a fixture file under firmware/test/fixtures/ regardless of the
// test binary's working directory. `pio test -e native` is expected to run from firmware/, so
// the plain relative path is tried first; if that can't be opened (e.g. run from elsewhere),
// falls back to a path built from this header's own __FILE__, which is stable no matter which
// test .cpp includes it (firmware/test/test_core/fixture_path.h -> up two levels -> +
// "fixtures/<name>").
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace transit_test {

inline std::string fixturePath(const std::string& name) {
  std::string rel = "test/fixtures/" + name;
  if (FILE* f = std::fopen(rel.c_str(), "rb")) {
    std::fclose(f);
    return rel;
  }

  std::string here = __FILE__;  // .../firmware/test/test_core/fixture_path.h
  size_t slash = here.find_last_of("/\\");
  if (slash != std::string::npos) here.erase(slash);  // .../firmware/test/test_core
  slash = here.find_last_of("/\\");
  if (slash != std::string::npos) here.erase(slash);  // .../firmware/test
  return here + "/fixtures/" + name;
}

// Reads an entire fixture file into memory. Aborts the test process with a clear message if the
// file can't be opened (fixtures are always small - a few KB to ~150 KB - so reading them whole
// in a test is fine even though the library under test must not do the same for the real feed).
inline std::vector<uint8_t> readFixture(const std::string& name) {
  std::string path = fixturePath(name);
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    std::fprintf(stderr, "fixture not found: %s (tried %s)\n", name.c_str(), path.c_str());
    std::exit(1);
  }
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  if (size > 0) {
    size_t n = std::fread(data.data(), 1, data.size(), f);
    (void)n;
  }
  std::fclose(f);
  return data;
}

}  // namespace transit_test
