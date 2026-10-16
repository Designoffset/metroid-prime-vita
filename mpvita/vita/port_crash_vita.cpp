// PS Vita: crashes are captured by the system as a core dump (ux0:data/*.psp2dmp),
// so the port's POSIX signal handler is replaced by this minimal version.
#include "port_crash.h"
#include "port_log.h"
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <typeinfo>

namespace PortCrash {
void Install() {}
bool RequestStack(long) { return false; }
[[noreturn]] void AbortOnException(const char* where) {
  const char* what = "unknown exception";
  try {
    if (auto e = std::current_exception()) std::rethrow_exception(e);
  } catch (const std::exception& ex) {
    what = ex.what();
  } catch (...) {
  }
  PortLog::Write("port: fatal exception in %s: %s\n", where ? where : "?", what);
  std::fflush(nullptr);
  std::abort();
}
} // namespace PortCrash
