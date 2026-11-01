#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <curl/curl.h>

#include "apiAuthMethod.hpp"
#include "authProvider/authConfig.hpp"
#include "authProvider/authCredentials.hpp"
#include "connectionOptions.hpp"
#include "environmentConfig.hpp"

/*
Everything needed to build a ConnectionConfig.

The driver previously passed a dozen positional strings to the constructor,
which made it easy to transpose two arguments of the same type. Grouping them
into named fields removes that class of bug now that the number of options has
grown.
*/
struct ConnectionSetup {
  std::string hostname       = "";
  unsigned short port        = 0;
  std::string connectionName = "";
  ApiAuthMethod authMethod   = AM_NO_AUTH;

  // OIDC parameters, used by the client-credentials and device-flow providers.
  std::string oidcDiscoveryUrl = "";
  std::string clientId         = "";
  std::string clientSecret     = "";
  std::string oidcScope        = "";
  std::string grantType        = "";
  std::string tokenEndpoint    = "";

  AuthCredentials credentials;
  ConnectionOptions options;
};

class ConnectionConfig {
  private:
    std::string hostname;
    unsigned short port;
    std::string connectionName;
    std::string tokenEndpoint;
    std::string grantType;

    ApiAuthMethod authMethod;
    std::unique_ptr<AuthConfig> authConfigPtr;
    std::vector<std::function<void(ConnectionConfig*)>> onDisconnectCallbacks;
    std::map<std::string, std::string> nonAuthHeaders;

    // Transport options (TLS verification, timeouts, client identification).
    ConnectionOptions options;

    // The user name supplied by the DSN or connection string, kept as a
    // fallback for getTrinoUser() when the auth provider has not yet resolved
    // an identity from a token.
    std::string configuredUser;

    // CURL is managed within the connection config. This way
    // we can set up all the right headers and SSL options
    // every time anything asks for a CURL handle.
    CURL* curl;

  public:
    ConnectionConfig(const ConnectionSetup& setup);

    ~ConnectionConfig();
    std::string const getHostname();
    std::string const getServerName();
    std::string const getStatementUrl();
    unsigned short const getPort();
    ApiAuthMethod const getAuthMethod();
    /*
    The identity this connection presents to Trino in X-Trino-User. It is not
    always the DSN's username: the JWT and OIDC providers can derive it from the
    token's claims, so this is the only value that answers "who am I really?"
    */
    std::string getTrinoUser();
    CURL* getCurl();
    long getLastHTTPStatusCode();
    void disconnect();
    std::string getTrinoServerVersion();
    void registerDisconnectCallback(std::function<void(ConnectionConfig*)> f);
    void unregisterDisconnectCallback(std::function<void(ConnectionConfig*)> f);
    void setRequestHeader(std::string key, std::string value);
    std::string getRequestHeader(std::string key);
    void clearRequestHeader(std::string key);

    // Making these public because they're frequently accessed and
    // manipulated external to the ConnectionConfig object
    std::string responseData;
    std::map<std::string, std::string> responseHeaderData;
};
