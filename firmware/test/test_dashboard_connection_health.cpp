#include "../src/dashboard_connection_health.h"

#include <cassert>

int main() {
  assert(!dashboardConnectionShouldStayOnline(false, 0, 0));
  assert(dashboardConnectionShouldStayOnline(true, 1, 60000));
  assert(dashboardConnectionShouldStayOnline(true, 2, 120000));
  assert(!dashboardConnectionShouldStayOnline(true, 3, 120000));
  assert(!dashboardConnectionShouldStayOnline(true, 2, 180000));
  return 0;
}
