#include "hostEnvironment.hpp"

#include <string>

#include "windowsLean.hpp"
#include "writeLog.hpp"

bool isInteractiveDesktopAvailable() {
  /*
  A Windows service runs in session 0, which since Vista has had no window
  station that a user can see. Anything running there cannot open a browser or
  show a prompt, so interactive authentication flows cannot complete.

  ProcessIdToSessionId is declared in winbase.h and implemented in kernel32,
  which is already linked.

  The check is deliberately conservative: if the session cannot be determined,
  the driver assumes it *is* interactive, because wrongly refusing to run a
  working desktop configuration is worse than allowing an interactive flow to
  hang in a service.
  */
  DWORD sessionId = 0;

  if (not ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) {
    WriteLog(LL_DEBUG,
             "  Could not determine process session id; assuming interactive.");
    return true;
  }

  bool interactive = sessionId != 0;
  if (not interactive) {
    WriteLog(LL_INFO,
             "  Running in a non-interactive session (session 0). Interactive "
             "authentication methods will be rejected.");
  }
  return interactive;
}
