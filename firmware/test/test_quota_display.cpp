#include "../src/quota_display.h"

#include <cassert>
#include <cstring>
#include <limits>

static void expectText(const QuotaDisplay& display, const char* expected, bool chinese = false) {
  char text[16] = {};
  formatQuotaRemaining(display, text, sizeof(text), chinese);
  assert(std::strcmp(text, expected) == 0);
}

int main() {
  // New payload: the integer field is floored by the collector, so exact wins.
  auto fractional = selectQuotaDisplay(false, true, 99.6, true, 99, false);
  assert(fractional.known && quotaRemaining(fractional) > 0);
  expectText(fractional, "<1%");
  expectText(selectQuotaDisplay(false, true, 100, true, 100, false), "0%");
  expectText(selectQuotaDisplay(false, true, 99, true, 99, false), "1%");

  // Old server payloads and unavailable windows retain their existing display.
  expectText(selectQuotaDisplay(false, false, 0, true, 99, false), "1%");
  expectText(selectQuotaDisplay(false, false, 0, true, 100, false), "0%");
  expectText(selectQuotaDisplay(false, false, 0, false, 0, false), "--%");

  auto conflict = selectQuotaDisplay(true, true, 100, true, 100, true);
  assert(conflict.conflict && !conflict.known);
  expectText(conflict, "sync");
  expectText(conflict, "更新中", true);

  // Malformed optional precision cannot conceal a valid legacy integer.
  expectText(selectQuotaDisplay(false, true, std::numeric_limits<double>::quiet_NaN(), true, 99, false), "1%");
  expectText(selectQuotaDisplay(false, true, std::numeric_limits<double>::infinity(), true, 99, false), "1%");
  expectText(selectQuotaDisplay(false, true, 101, true, 99, false), "1%");
  expectText(selectQuotaDisplay(false, true, -1, true, 99, false), "1%");
  return 0;
}
