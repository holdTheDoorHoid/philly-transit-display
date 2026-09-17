// The one translation unit that replaces global operator new/delete. See alloc_probe.h.
#include "alloc_probe.h"

#include <cstdlib>
#include <new>

namespace {
bool g_tracking = false;
size_t g_largest = 0;
size_t g_count = 0;

void note(size_t n) {
  if (!g_tracking) return;
  if (n > g_largest) g_largest = n;
  ++g_count;
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
  g_tracking = true;
}

size_t AllocProbe::end() {
  g_tracking = false;
  return g_largest;
}

size_t AllocProbe::count() { return g_count; }

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
