// harness.hpp — a dependency-free test harness.
//
// ziptastic must build with nothing but CMake and a C++20 compiler, so the
// tests bring their own registry and assertions rather than pulling in gtest.
//
//   TEST(name) { CHECK(x); }   registers and runs `name`
//   TEST_MAIN()                provides main() for a single test executable
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace chce_test {

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

inline int& failure_count() {
  static int f = 0;
  return f;
}

inline const char*& current_test() {
  static const char* n = "";
  return n;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void report_failure(const char* file, int line, const std::string& what) {
  std::fprintf(stderr, "  FAIL %s (%s:%d): %s\n", current_test(), file, line,
               what.c_str());
  ++failure_count();
}

inline int run_all() {
  int ran = 0;
  for (const TestCase& t : registry()) {
    const int before = failure_count();
    current_test() = t.name;
    t.fn();
    ++ran;
    if (failure_count() != before) {
      std::fprintf(stderr, "[FAIL] %s\n", t.name);
    } else {
      std::fprintf(stderr, "[ ok ] %s\n", t.name);
    }
  }
  if (failure_count() == 0) {
    std::fprintf(stderr, "all %d test case(s) passed\n", ran);
    return 0;
  }
  std::fprintf(stderr, "%d assertion(s) failed across %d test case(s)\n",
               failure_count(), ran);
  return 1;
}

// Deterministic pseudo-random source (xorshift64*), so failures reproduce.
class Rng {
 public:
  explicit Rng(uint64_t seed) : s_(seed ? seed : 0x9E3779B97F4A7C15ull) {}

  uint64_t next() {
    s_ ^= s_ >> 12;
    s_ ^= s_ << 25;
    s_ ^= s_ >> 27;
    return s_ * 0x2545F4914F6CDD1Dull;
  }

  // Uniform in [0, n).
  uint64_t below(uint64_t n) { return n ? next() % n : 0; }

  uint8_t byte() { return static_cast<uint8_t>(next() >> 33); }

 private:
  uint64_t s_;
};

}  // namespace chce_test

#define TEST(name)                                                    \
  static void name();                                                 \
  static ::chce_test::Registrar registrar_##name(#name, name);        \
  static void name()

#define TEST_MAIN()                    \
  int main() { return ::chce_test::run_all(); }

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) ::chce_test::report_failure(__FILE__, __LINE__, #cond);   \
  } while (0)

#define CHECK_MSG(cond, msg)                                                  \
  do {                                                                        \
    if (!(cond))                                                              \
      ::chce_test::report_failure(__FILE__, __LINE__,                         \
                                  std::string(#cond) + " — " + (msg));         \
  } while (0)

#define CHECK_EQ(a, b)                                                        \
  do {                                                                        \
    const auto _va = (a);                                                     \
    const auto _vb = (b);                                                     \
    if (!(_va == _vb))                                                        \
      ::chce_test::report_failure(__FILE__, __LINE__,                          \
                                  std::string(#a) + " == " + #b + " (" +       \
                                      std::to_string(_va) + " vs " +           \
                                      std::to_string(_vb) + ")");              \
  } while (0)

#define CHECK_ERR(expr, expected)                                             \
  do {                                                                        \
    const ::chce::Err _e = (expr);                                            \
    if (_e != (expected))                                                     \
      ::chce_test::report_failure(                                            \
          __FILE__, __LINE__,                                                 \
          std::string(#expr) + " returned " + ::chce::err_name(_e) +          \
              ", expected " + ::chce::err_name(expected));                    \
  } while (0)
