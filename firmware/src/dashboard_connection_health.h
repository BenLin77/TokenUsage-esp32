#pragma once

static constexpr unsigned int DASHBOARD_OFFLINE_FAILURE_LIMIT = 3;
static constexpr unsigned long DASHBOARD_OFFLINE_GRACE_MS = 3UL * 60UL * 1000UL;

constexpr bool dashboardConnectionShouldStayOnline(bool hasSuccessfulFetch,
                                                    unsigned int consecutiveFailures,
                                                    unsigned long msSinceSuccess) {
  return hasSuccessfulFetch &&
         consecutiveFailures < DASHBOARD_OFFLINE_FAILURE_LIMIT &&
         msSinceSuccess < DASHBOARD_OFFLINE_GRACE_MS;
}
