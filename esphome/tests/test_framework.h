// Minimal self-contained test framework for the OpenNeato ESPHome host tests.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace tf {

struct TestCase {
  const char *name;
  std::function<void()> fn;
};

inline std::vector<TestCase> &registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int &failures() {
  static int count = 0;
  return count;
}

struct Registrar {
  Registrar(const char *name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline std::string repr(const std::string &v) { return "\"" + v + "\""; }
inline std::string repr(const char *v) { return v == nullptr ? "nullptr" : "\"" + std::string(v) + "\""; }
inline std::string repr(bool v) { return v ? "true" : "false"; }
template<typename T> std::string repr(const T &v) { return std::to_string(v); }

}  // namespace tf

#define TF_CONCAT2(a, b) a##b
#define TF_CONCAT(a, b) TF_CONCAT2(a, b)

#define TEST(name) \
  static void name(); \
  static tf::Registrar TF_CONCAT(tf_reg_, name)(#name, name); \
  static void name()

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("    FAIL %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
      tf::failures()++; \
    } \
  } while (0)

#define CHECK_EQ(actual, expected) \
  do { \
    auto tf_a = (actual); \
    auto tf_e = (expected); \
    if (!(tf_a == tf_e)) { \
      std::printf("    FAIL %s:%d: %s == %s\n      actual:   %s\n      expected: %s\n", __FILE__, __LINE__, #actual, \
                  #expected, tf::repr(tf_a).c_str(), tf::repr(tf_e).c_str()); \
      tf::failures()++; \
    } \
  } while (0)

#define CHECK_STR(actual, expected) CHECK_EQ(std::string(actual), std::string(expected))

#define CHECK_NEAR(actual, expected, eps) \
  do { \
    double tf_a = (actual); \
    double tf_e = (expected); \
    if (std::fabs(tf_a - tf_e) > (eps)) { \
      std::printf("    FAIL %s:%d: %s ~= %s (actual %f, expected %f)\n", __FILE__, __LINE__, #actual, #expected, tf_a, \
                  tf_e); \
      tf::failures()++; \
    } \
  } while (0)
