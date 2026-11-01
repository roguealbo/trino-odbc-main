#pragma once

#include <string>

/*
Credential material shared by the authentication providers.

This lives in the API wrapper layer (rather than reusing DriverConfig from the
driver layer) so the wrapper keeps no dependency on the driver's DSN parsing
code. The driver fills this in from the DSN and connection string and hands it
to ConnectionConfig.
*/
struct AuthCredentials {
  // Used by Basic and Kerberos auth, and as X-Trino-User for No Auth.
  std::string username = "";
  // Used by Basic auth.
  std::string password = "";
  // Used by JWT auth: exactly one of these three is set.
  std::string token          = "";
  std::string tokenFile      = "";
  std::string tokenCommand   = "";
  // Used by Kerberos auth: e.g. "trino/coordinator.example.com".
  std::string servicePrincipalName = "";
};
