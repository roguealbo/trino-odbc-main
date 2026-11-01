#pragma once

/*
Host-environment probing.

The driver can be hosted by an interactive desktop application (Excel, Power BI
Desktop) or by a Windows service (the Power BI on-premises data gateway). Those
two hosting models differ in one way that matters a great deal to this driver:
a service has no window station or desktop, so any authentication flow that
needs to open a browser or wait for a human being will hang until the request
times out.

Detecting the hosting model lets the driver reject an impossible configuration
at connect time with an actionable message, instead of hanging.
*/

/*
Returns true when the current process can present UI to a human.

Services run in session 0 and have no interactive desktop; ordinary desktop
applications run in session 1 or higher. Session 0 isolation has been the
Windows behaviour since Vista, so a session id of 0 is a reliable signal that
no browser can be launched for an interactive auth flow.
*/
bool isInteractiveDesktopAvailable();
