#include "basicAuthProvider.hpp"

#include <map>
#include <stdexcept>
#include <string>

#include "../../util/cryptUtils.hpp"
#include "../../util/writeLog.hpp"

/*
HTTP Basic authentication.

This is the simplest non-interactive authentication method, and the one most
often used with the Microsoft Power BI on-premises data gateway: the gateway
service account has no desktop session, so it cannot complete a browser based
flow, but it can send a username and password that live in the DSN.

Trino expects:
  * "Authorization: Basic base64(username:password)"
  * "X-Trino-User: <username>"

Note that Trino only accepts Basic credentials over HTTPS, so the DSN should
point at the coordinator's TLS port.
*/
class BasicAuthConfig : public AuthConfig {
  public:
    BasicAuthConfig(std::string hostname,
                    unsigned short port,
                    std::string connectionName,
                    std::string username,
                    std::string password)
        : AuthConfig(hostname, port, connectionName) {
      if (username.empty()) {
        throw std::invalid_argument(
            "username is required for Basic authentication");
      }

      this->trinoUser = username;
      this->headers.insert({"X-Trino-User", username});

      std::string credentials = username + ":" + password;
      this->headers.insert(
          {"Authorization", "Basic " + base64Encode(credentials)});

      WriteLog(LL_INFO,
               "  Basic authentication configured for user: " + username);
    }

    bool const isExpired() override {
      // Static credentials: there is never anything to refresh.
      return false;
    }

    void
    refresh(CURL* curl,
            std::string* responseData,
            std::map<std::string, std::string>* responseHeaderData) override {
      // There's nothing to refresh.
    }

    ~BasicAuthConfig() override = default;
};

std::unique_ptr<AuthConfig> getBasicAuthConfigPtr(std::string hostname,
                                                  unsigned short port,
                                                  std::string connectionName,
                                                  std::string username,
                                                  std::string password) {
  return std::make_unique<BasicAuthConfig>(
      hostname, port, connectionName, username, password);
}
