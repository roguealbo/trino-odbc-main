#include "noAuthProvider.hpp"

#include <map>
#include <string>

#include "../../util/writeLog.hpp"

// When no username is configured we fall back to this, which keeps the
// behaviour the bundled test suite expects against a vanilla Trino instance.
static const std::string NO_AUTH_DEFAULT_USER = "TestUser";

/*
No authentication. The coordinator is running with the default (insecure)
authenticator, so only X-Trino-User is required.

The username now comes from the DSN or connection string instead of being
hard-coded, which is what lets a Power BI gateway DSN present a real identity
to Trino's OPA-based access control.
*/
class NoAuthAuthConfig : public AuthConfig {
  public:
    NoAuthAuthConfig(std::string hostname,
                     unsigned short port,
                     std::string connectionName,
                     const AuthCredentials& creds)
        : AuthConfig(hostname, port, connectionName) {
      std::string user =
          creds.username.empty() ? NO_AUTH_DEFAULT_USER : creds.username;
      this->trinoUser                = user;
      this->headers["X-Trino-User"]  = user;
      WriteLog(LL_INFO, "  No-auth connection configured for user: " + user);
    }

    bool const isExpired() override {
      return false;
    }
    void
    refresh(CURL* curl,
            std::string* responseData,
            std::map<std::string, std::string>* responseHeaderData) override {
      // There's nothing to refresh.
    }
    ~NoAuthAuthConfig() override = default;
};

std::unique_ptr<AuthConfig> getNoAuthConfigPtr(std::string hostname,
                                               unsigned short port,
                                               std::string connectionName,
                                               const AuthCredentials& creds) {
  return std::make_unique<NoAuthAuthConfig>(
      hostname, port, connectionName, creds);
}
