// The one translation unit that replaces global operator new/delete. See alloc_probe.h.
#include "alloc_probe.h"

#include <cstdlib>
#include <new>

namespace {
bool g_tracking = false;
size_t g_largest = 0;
size_t g_count = 0;
// Every request of at least 1 KB made inside the window, in order. A poll cycle makes a handful,
// so a fixed array is plenty and keeps the probe itself from allocating.
constexpr size_t kMaxBig = 64;
size_t g_big[kMaxBig];
size_t g_big_count = 0;

void note(size_t n) {
  if (!g_tracking) return;
  if (n > g_largest) g_largest = n;
  ++g_count;
  if (n >= 1024 && g_big_count < kMaxBig) g_big[g_big_count++] = n;
}

void* alloc(size_t n) {
  note(n);
  void* p = std::malloc(n == 0 ? 1 : n);  // malloc(0) may legitimately return null
  return p;
}
}  // namespace

namespace transit_test {

void AllocProbe::begin() {
  g_largest = 0;
  g_count = 0;
  g_big_count = 0;
  g_tracking = true;
}

size_t AllocProbe::end() {
  g_tracking = false;
  return g_largest;
}

size_t AllocProbe::count() { return g_count; }

size_t AllocProbe::countAtLeast(size_t bytes) {
  size_t n = 0;
  for (size_t i = 0; i < g_big_count; ++i) {
    if (g_big[i] >= bytes) ++n;
  }
  return n;
}

}  // namespace transit_test

void* operator new(size_t n) {
  void* p = alloc(n);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void* operator new[](size_t n) {
  void* p = alloc(n);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void* operator new(size_t n, const std::nothrow_t&) noexcept { return alloc(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return alloc(n); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
