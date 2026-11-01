#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../../trinoAPIWrapper/apiAuthMethod.hpp"
#include "../../trinoAPIWrapper/authProvider/authCredentials.hpp"
#include "../../trinoAPIWrapper/connectionOptions.hpp"
#include "../../util/writeLog.hpp"


extern std::map<std::string, std::string> DRIVER_CONFIG_DEFAULT_VALUES;
extern std::vector<std::string> LOG_LEVEL_NAMES;
extern std::vector<std::string> AUTH_METHOD_NAMES;

class DriverConfig {
  private:
    // Actual configuration values.
    std::string dsn              = "";
    std::string driver           = "";
    std::string hostname         = "";
    uint16_t port                = 0;
    LogLevel logLevel            = LL_NONE;
    ApiAuthMethod authMethod     = AM_NO_AUTH;
    std::string oidcDiscoveryUrl = "";
    std::string clientId         = "";
    std::string clientSecret     = "";
    std::string oidcScope        = "";
    std::string tokenEndpoint    = "";
    std::string grantType        = "";

    // Credentials for the non-interactive auth methods. These are what make
    // the driver usable from the Power BI on-premises data gateway, which has
    // no desktop session available for an interactive flow.
    AuthCredentials credentials;

    // Transport / client-identification options.
    ConnectionOptions options;

    // Metadata describing the status of this config object.
    bool isSaved = false;

  public:
    std::string getDSN();
    void setDSN(std::string dsn);

    std::string getDriver();
    void setDriver(std::string driver);

    std::string getHostname();
    void setHostname(std::string hostname);

    std::string getPortStr();
    uint16_t getPortNum();
    void setPort(std::string port);
    void setPort(uint16_t port);

    std::string getLogLevelStr();
    LogLevel getLogLevelEnum();
    void setLogLevel(LogLevel level);
    void setLogLevel(std::string level);

    std::string getAuthMethodStr();
    ApiAuthMethod getAuthMethodEnum();
    void setAuthMethod(std::string authMethod);
    void setAuthMethod(ApiAuthMethod authMethod);

    std::string getOidcDiscoveryUrl();
    void setOidcDiscoveryUrl(std::string oidcDiscoveryUrl);

    std::string getClientId();
    void setClientId(std::string clientId);

    std::string getClientSecret();
    void setClientSecret(std::string clientSecret);

    std::string getOidcScope();
    void setOidcScope(std::string oidcScope);

    bool getIsSaved();
    void setIsSaved(bool isSaved);

    std::string getTokenEndpoint();
    void setTokenEndpoint(std::string tokenEndpoint);

    std::string getGrantType();
    void setGrantType(std::string grantType);

    // Credentials
    std::string getUsername();
    void setUsername(std::string username);

    std::string getPassword();
    void setPassword(std::string password);

    std::string getToken();
    void setToken(std::string token);

    std::string getTokenFile();
    void setTokenFile(std::string tokenFile);

    std::string getTokenCommand();
    void setTokenCommand(std::string tokenCommand);

    std::string getServicePrincipalName();
    void setServicePrincipalName(std::string servicePrincipalName);

    // Transport options
    std::string getSslCAInfo();
    void setSslCAInfo(std::string sslCAInfo);

    bool getSslVerify();
    void setSslVerify(bool sslVerify);
    void setSslVerify(std::string sslVerify);

    long getRequestTimeoutMs();
    void setRequestTimeoutMs(long requestTimeoutMs);
    void setRequestTimeoutMs(std::string requestTimeoutMs);

    std::string getClientInfo();
    void setClientInfo(std::string clientInfo);

    // Bulk accessors used when handing configuration to the API wrapper.
    AuthCredentials getCredentials();
    ConnectionOptions getOptions();

    std::string serialize();
    static DriverConfig deserialize(const std::string& jsonStr);
};

DriverConfig driverConfigFromKVPs(std::map<std::string, std::string> kvps);
std::map<std::string, std::string> driverConfigToKVPs(DriverConfig config);
