#pragma once

enum ApiAuthMethod {
  AM_NO_AUTH          = 0,
  AM_EXTERNAL_AUTH    = 1,
  AM_CLIENT_CRED_AUTH = 2,
  AM_DEVICE_FLOW      = 3,
  // The methods below are all non-interactive. They are the ones that work
  // when the driver is hosted by a Windows service with no desktop session,
  // such as the Microsoft Power BI on-premises data gateway.
  AM_BASIC_AUTH    = 4,
  AM_TOKEN_AUTH    = 5,
  AM_KERBEROS_AUTH = 6
};

/*
Returns true when the auth method can complete without user interaction
(no browser window, no console prompt). Callers use this to reject
configurations that can never succeed in a headless host process.
*/
inline bool isNonInteractiveAuthMethod(ApiAuthMethod method) {
  switch (method) {
    case AM_NO_AUTH:
    case AM_CLIENT_CRED_AUTH:
    case AM_BASIC_AUTH:
    case AM_TOKEN_AUTH:
    case AM_KERBEROS_AUTH: {
      return true;
    }
    case AM_EXTERNAL_AUTH:
    case AM_DEVICE_FLOW: {
      return false;
    }
  }
  return false;
}
