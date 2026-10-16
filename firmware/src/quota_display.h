#pragma once

#include <cmath>
#include <cstdio>

struct QuotaDisplay {
  bool known = false;
  bool conflict = false;
  bool hasExact = false;
  int usedPct = 0;
  double exactUsedPct = 0;
};

inline QuotaDisplay selectQuotaDisplay(bool conflict, bool hasExact, double exact,
                                       bool hasLegacy, int legacy, bool exhausted) {
  QuotaDisplay result;
  result.conflict = conflict;
  if (conflict) return result;
  if (hasExact && std::isfinite(exact) && exact >= 0 && exact <= 100) {
    result.known = true;
    result.hasExact = true;
    result.exactUsedPct = exact;
    result.usedPct = static_cast<int>(exact);
  } else if (hasLegacy) {
    result.known = true;
    result.usedPct = legacy < 0 ? 0 : (legacy > 100 ? 100 : legacy);
  } else if (exhausted) {
    result.known = true;
    result.usedPct = 100;
  }
  return result;
}

inline double quotaRemaining(const QuotaDisplay& quota) {
  if (!quota.known) return -1;
  return 100.0 - (quota.hasExact ? quota.exactUsedPct : quota.usedPct);
}

inline void formatQuotaRemaining(const QuotaDisplay& quota, char* out, size_t size,
                                 bool chinese = false) {
  if (size == 0) return;
  if (quota.conflict) {
    std::snprintf(out, size, "%s", chinese ? "更新中" : "sync");
    return;
  }
  if (!quota.known) {
    std::snprintf(out, size, "--%%");
    return;
  }
  const double remaining = quotaRemaining(quota);
  if (remaining > 0 && remaining < 1) {
    std::snprintf(out, size, "<1%%");
  } else {
    std::snprintf(out, size, "%d%%", static_cast<int>(remaining));
  }
}
