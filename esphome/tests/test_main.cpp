#include "test_framework.h"

int main(int argc, char **argv) {
  const char *filter = argc > 1 ? argv[1] : nullptr;
  int run = 0;
  for (const auto &t : tf::registry()) {
    if (filter != nullptr && std::string(t.name).find(filter) == std::string::npos)
      continue;
    int before = tf::failures();
    t.fn();
    run++;
    std::printf("%s %s\n", tf::failures() == before ? "  ok  " : "  FAIL", t.name);
  }
  std::printf("\n%d tests, %d failed checks\n", run, tf::failures());
  return tf::failures() == 0 ? 0 : 1;
}
