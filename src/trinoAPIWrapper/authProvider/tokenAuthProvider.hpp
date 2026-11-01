#pragma once

#include <memory>
#include <string>

#include "authConfig.hpp"

/*
Obtains an AuthConfig for JWT (bearer token) authentication.

Three mutually exclusive ways to supply the token, in order of precedence:
  1. token        - a JWT provided directly in the DSN or connection string
  2. tokenFile    - path to a file containing the JWT, re-read when it expires
  3. tokenCommand - a command line that prints the JWT to stdout, re-run when
                    it expires (useful for fetching from Vault, a sidecar, or a
                    cloud CLI)

All three are non-interactive, so they work under the Power BI on-premises
data gateway service account.
*/
std::unique_ptr<AuthConfig> getTokenAuthConfigPtr(std::string hostname,
                                                  unsigned short port,
                                                  std::string connectionName,
                                                  std::string username,
                                                  std::string token,
                                                  std::string tokenFile,
                                                  std::string tokenCommand);
