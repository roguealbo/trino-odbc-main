#include "driverConfig.hpp"
#include "../../util/capitalize.hpp"
#include "../../util/writeLog.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <stdexcept>
#include <string>


std::vector<std::string> LOG_LEVEL_NAMES = {
    "None", "Error", "Warn", "Info", "Debug", "Trace"};

std::vector<LogLevel> LOG_LEVEL_VALUES = {
    LL_NONE, LL_ERROR, LL_WARN, LL_INFO, LL_DEBUG, LL_TRACE};

std::map<LogLevel, std::string> LOG_LEVEL_TO_LOG_NAME = {
    std::make_pair(LL_TRACE, "Trace"),
    std::make_pair(LL_DEBUG, "Debug"),
    std::make_pair(LL_INFO, "Info"),
    std::make_pair(LL_WARN, "Warn"),
    std::make_pair(LL_ERROR, "Error"),
    std::make_pair(LL_NONE, "None"),
};

std::map<std::string, LogLevel> LOG_NAME_TO_LOG_LEVEL = {
    std::make_pair("Trace", LL_TRACE),
    std::make_pair("Debug", LL_DEBUG),
    std::make_pair("Info", LL_INFO),
    std::make_pair("Warn", LL_WARN),
    std::make_pair("Error", LL_ERROR),
    std::make_pair("None", LL_NONE),
};

/*
The order of these vectors defines the order the options appear in the DSN
configuration form's Auth Method dropdown. The three non-interactive methods
added last are the ones that work under the Power BI on-premises data gateway.
*/
std::vector<std::string> AUTH_METHOD_NAMES = {"No Auth",
                                              "External Auth",
                                              "OIDC Client Cred Auth",
                                              "Device Flow",
                                              "Basic Auth",
                                              "Token Auth",
                                              "Kerberos Auth"};

std::vector<ApiAuthMethod> AUTH_METHOD_VALUES = {AM_NO_AUTH,
                                                 AM_EXTERNAL_AUTH,
                                                 AM_CLIENT_CRED_AUTH,
                                                 AM_DEVICE_FLOW,
                                                 AM_BASIC_AUTH,
                                                 AM_TOKEN_AUTH,
                                                 AM_KERBEROS_AUTH};

std::map<ApiAuthMethod, std::string> AUTH_METHOD_TO_AUTH_NAME = {
    std::make_pair(AM_NO_AUTH, "No Auth"),
    std::make_pair(AM_EXTERNAL_AUTH, "External Auth"),
    std::make_pair(AM_CLIENT_CRED_AUTH, "OIDC Client Cred Auth"),
    std::make_pair(AM_DEVICE_FLOW, "Device Flow"),
    std::make_pair(AM_BASIC_AUTH, "Basic Auth"),
    std::make_pair(AM_TOKEN_AUTH, "Token Auth"),
    std::make_pair(AM_KERBEROS_AUTH, "Kerberos Auth")};

/*
Keys here must be the result of running the display name through
capitalizedCase(), which lowercases everything and then uppercases the first
letter of each space-delimited word. That is why "OIDC Client Cred Auth"
appears as "Oidc Client Cred Auth".
*/
std::map<std::string, ApiAuthMethod> AUTH_NAME_TO_AUTH_METHOD = {
    std::make_pair("No Auth", AM_NO_AUTH),
    std::make_pair("External Auth", AM_EXTERNAL_AUTH),
    std::make_pair("Oidc Client Cred Auth", AM_CLIENT_CRED_AUTH),
    std::make_pair("Device Flow", AM_DEVICE_FLOW),
    std::make_pair("Basic Auth", AM_BASIC_AUTH),
    std::make_pair("Token Auth", AM_TOKEN_AUTH),
    std::make_pair("Kerberos Auth", AM_KERBEROS_AUTH)};

/*
The valid auth method names, for error messages. Listing them is what turns
"unknown authentication method" into something an administrator can act on
without opening the README.
*/
static std::string joinAuthMethodNames() {
  std::string names;
  for (std::string name : AUTH_METHOD_NAMES) {
    if (not names.empty()) {
      names += ", ";
    }
    names += name;
  }
  return names;
}


// All default values - do not miss any!
std::map<std::string, std::string> DRIVER_CONFIG_DEFAULT_VALUES = {
    std::make_pair("hostname", "localhost"),
    std::make_pair("port", "8080"),
    std::make_pair("loglevel", "None"),
    std::make_pair("authmethod", "No Auth"),
    std::make_pair("oidcDiscoveryUrl", ""),
    std::make_pair("clientId", ""),
    std::make_pair("clientSecret", ""),
    std::make_pair("oidcScope", ""),
    std::make_pair("secretEncryptionLevel", "user"),
    std::make_pair("username", ""),
    std::make_pair("password", ""),
    std::make_pair("token", ""),
    std::make_pair("tokenFile", ""),
    std::make_pair("tokenCommand", ""),
    std::make_pair("servicePrincipalName", ""),
    std::make_pair("sslCAInfo", ""),
    std::make_pair("sslVerify", "true"),
    std::make_pair("requestTimeoutMs", "10000"),
    std::make_pair("clientInfo", ""),
};

// DSN
std::string DriverConfig::getDSN() {
  return this->dsn;
}
void DriverConfig::setDSN(std::string dsn) {
  this->dsn = dsn;
}

// Driver
std::string DriverConfig::getDriver() {
  return this->driver;
}
void DriverConfig::setDriver(std::string driver) {
  this->driver = driver;
}

// Hostname
std::string DriverConfig::getHostname() {
  return this->hostname;
}
void DriverConfig::setHostname(std::string hostname) {
  this->hostname = hostname;
}

/*
Parse a number from a DSN or connection-string value without throwing.

These setters are called from the DSN configuration dialog's window procedure
and from ConfigDSN, both of which run inside the Win32 message loop. An
exception thrown there unwinds through DispatchMessage and terminates the host
process, so unparseable input has to be handled here: the value is left at its
default and the problem is reported in the log.
*/
static long parseLongOrDefault(const std::string& text, long defaultValue) {
  if (text.empty()) {
    return defaultValue;
  }
  try {
    size_t charactersRead = 0;
    long parsed           = std::stol(text, &charactersRead);
    if (charactersRead != text.size()) {
      WriteLog(LL_WARN,
               "  Ignoring trailing characters in numeric setting: " + text);
    }
    return parsed;
  } catch (const std::exception&) {
    WriteLog(LL_WARN, "  Could not parse numeric setting: '" + text + "'");
    return defaultValue;
  }
}

// Port - Accepts and returns both integers and strings.
std::string DriverConfig::getPortStr() {
  return std::to_string(this->port);
}
uint16_t DriverConfig::getPortNum() {
  return this->port;
}
void DriverConfig::setPort(std::string port) {
  long parsed = parseLongOrDefault(port, this->port);
  /*
  Clamp to the valid range rather than letting a wide value wrap silently into a
  uint16_t, which would turn port 80800 into 14464 and produce a baffling
  connection failure.
  */
  if (parsed < 0 or parsed > 65535) {
    WriteLog(LL_WARN, "  Port out of range: " + port);
    return;
  }
  this->port = static_cast<uint16_t>(parsed);
}
void DriverConfig::setPort(uint16_t port) {
  this->port = port;
}

// Log Level
std::string DriverConfig::getLogLevelStr() {
  return LOG_LEVEL_TO_LOG_NAME.at(this->logLevel);
}
LogLevel DriverConfig::getLogLevelEnum() {
  return this->logLevel;
}
void DriverConfig::setLogLevel(LogLevel level) {
  this->logLevel = level;
}
void DriverConfig::setLogLevel(std::string level) {
  /*
  Fall back to the current value rather than throwing. This is called while
  reading a DSN inside ConfigDSN and inside the dialog's window procedure, where
  an exception would unwind through the Win32 message loop and take the host
  process with it. A hand-edited ODBC.INI with a typo in loglevel should produce
  a warning, not a crash.
  */
  std::string casedLevel = capitalizedCase(level);
  auto it                = LOG_NAME_TO_LOG_LEVEL.find(casedLevel);
  if (it == LOG_NAME_TO_LOG_LEVEL.end()) {
    WriteLog(LL_WARN, "  Unknown log level: '" + level + "'; keeping " +
                          getLogLevelStr());
    return;
  }
  this->logLevel = it->second;
}

// Auth Method
std::string DriverConfig::getAuthMethodStr() {
  return AUTH_METHOD_TO_AUTH_NAME.at(this->authMethod);
}
ApiAuthMethod DriverConfig::getAuthMethodEnum() {
  return this->authMethod;
}
void DriverConfig::setAuthMethod(ApiAuthMethod authMethod) {
  this->authMethod = authMethod;
}
void DriverConfig::setAuthMethod(std::string authMethod) {
  /*
  Report an unrecognised method as an invalid argument rather than letting
  map::at() raise std::out_of_range, and do *not* fall back to a default.
  Silently keeping "No Auth" when the administrator typed "Kerberos Authm"
  would connect unauthenticated, which is far worse than refusing to connect.
  The caller turns this into an SQL_ERROR with the text below.
  */
  std::string casedAuthMethod = capitalizedCase(authMethod);
  auto it                     = AUTH_NAME_TO_AUTH_METHOD.find(casedAuthMethod);
  if (it == AUTH_NAME_TO_AUTH_METHOD.end()) {
    throw std::invalid_argument("unknown authentication method '" + authMethod +
                                "'. Valid values are: " +
                                joinAuthMethodNames());
  }
  this->authMethod = it->second;
}

// OIDC Discovery URL
std::string DriverConfig::getOidcDiscoveryUrl() {
  return this->oidcDiscoveryUrl;
}
void DriverConfig::setOidcDiscoveryUrl(std::string oidcDiscoveryUrl) {
  this->oidcDiscoveryUrl = oidcDiscoveryUrl;
}

// Client ID
std::string DriverConfig::getClientId() {
  return this->clientId;
}
void DriverConfig::setClientId(std::string clientId) {
  this->clientId = clientId;
}

// Client Secret
std::string DriverConfig::getClientSecret() {
  return this->clientSecret;
}
void DriverConfig::setClientSecret(std::string clientSecret) {
  this->clientSecret = clientSecret;
}

// OIDC Scope
std::string DriverConfig::getOidcScope() {
  return this->oidcScope;
}
void DriverConfig::setOidcScope(std::string oidcScope) {
  this->oidcScope = oidcScope;
}

std::string DriverConfig::getTokenEndpoint() {
  return this->tokenEndpoint;
}
void DriverConfig::setTokenEndpoint(std::string tokenEndpoint) {
  this->tokenEndpoint = tokenEndpoint;
}

std::string DriverConfig::getGrantType() {
  return this->grantType;
}
void DriverConfig::setGrantType(std::string grantType) {
  this->grantType = grantType;
}

// Username - the identity Trino sees, sent as X-Trino-User.
std::string DriverConfig::getUsername() {
  return this->credentials.username;
}
void DriverConfig::setUsername(std::string username) {
  this->credentials.username = username;
}

// Password - Basic auth only.
std::string DriverConfig::getPassword() {
  return this->credentials.password;
}
void DriverConfig::setPassword(std::string password) {
  this->credentials.password = password;
}

// Token - a JWT supplied directly.
std::string DriverConfig::getToken() {
  return this->credentials.token;
}
void DriverConfig::setToken(std::string token) {
  this->credentials.token = token;
}

// Token File - path to a file holding a JWT.
std::string DriverConfig::getTokenFile() {
  return this->credentials.tokenFile;
}
void DriverConfig::setTokenFile(std::string tokenFile) {
  this->credentials.tokenFile = tokenFile;
}

// Token Command - a command line that prints a JWT to stdout.
std::string DriverConfig::getTokenCommand() {
  return this->credentials.tokenCommand;
}
void DriverConfig::setTokenCommand(std::string tokenCommand) {
  this->credentials.tokenCommand = tokenCommand;
}

// Kerberos service principal name.
std::string DriverConfig::getServicePrincipalName() {
  return this->credentials.servicePrincipalName;
}
void DriverConfig::setServicePrincipalName(std::string servicePrincipalName) {
  this->credentials.servicePrincipalName = servicePrincipalName;
}

// TLS CA bundle path.
std::string DriverConfig::getSslCAInfo() {
  return this->options.sslCAInfo;
}
void DriverConfig::setSslCAInfo(std::string sslCAInfo) {
  this->options.sslCAInfo = sslCAInfo;
}

// TLS verification toggle.
bool DriverConfig::getSslVerify() {
  return this->options.sslVerify;
}
void DriverConfig::setSslVerify(bool sslVerify) {
  this->options.sslVerify = sslVerify;
}
void DriverConfig::setSslVerify(std::string sslVerify) {
  /*
  Accept the same truthy spellings the ClickHouse driver accepts for its
  SSLMode-style options, so a DSN written by hand or by a Power BI template
  behaves predictably. Anything unrecognised is treated as true, which is the
  safe direction to fail in.
  */
  std::string lowered = sslVerify;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
  this->options.sslVerify =
      not(lowered == "false" || lowered == "off" || lowered == "no" ||
          lowered == "0");
}

// Per-request timeout.
long DriverConfig::getRequestTimeoutMs() {
  return this->options.requestTimeoutMs;
}
void DriverConfig::setRequestTimeoutMs(long requestTimeoutMs) {
  this->options.requestTimeoutMs = requestTimeoutMs;
}
void DriverConfig::setRequestTimeoutMs(std::string requestTimeoutMs) {
  long parsed =
      parseLongOrDefault(requestTimeoutMs, this->options.requestTimeoutMs);
  /*
  A zero or negative timeout would make curl fail every request immediately,
  which reads as "the driver is broken" rather than "the DSN has a typo".
  */
  if (parsed <= 0) {
    WriteLog(LL_WARN,
             "  Ignoring non-positive requestTimeoutMs: " + requestTimeoutMs);
    return;
  }
  this->options.requestTimeoutMs = parsed;
}

// Client identification string.
std::string DriverConfig::getClientInfo() {
  return this->options.clientInfo;
}
void DriverConfig::setClientInfo(std::string clientInfo) {
  this->options.clientInfo = clientInfo;
}

AuthCredentials DriverConfig::getCredentials() {
  return this->credentials;
}

ConnectionOptions DriverConfig::getOptions() {
  return this->options;
}

// IsSaved
bool DriverConfig::getIsSaved() {
  return this->isSaved;
}
void DriverConfig::setIsSaved(bool isSaved) {
  this->isSaved = isSaved;
}

/*
Read a key from the KVP map under either its camelCase (DSN) or lowercase
(connection string) spelling.

KVPs coming from a DSN entry use (mostly) camelCase. KVPs coming from a
connection string are lowercased by the parser. Connection string values must
win over DSN values, so the lowercase lookup is applied last.
*/
static void
applyKVP(std::map<std::string, std::string>& kvps,
         const std::string& camelKey,
         const std::function<void(const std::string&)>& setter) {
  /*
  Empty values are skipped. A connection string may legitimately contain an
  empty value ("TrinoDSN;port=;"), and the numeric and enum setters would throw
  on an empty string. Skipping leaves the value at its default instead.
  */
  auto camelIt = kvps.find(camelKey);
  if (camelIt != kvps.end() and not camelIt->second.empty()) {
    setter(camelIt->second);
  }
  std::string lowerKey = camelKey;
  std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);
  auto lowerIt = kvps.find(lowerKey);
  if (lowerIt != kvps.end() and not lowerIt->second.empty()) {
    setter(lowerIt->second);
  }
}

// Create a config from key value pairs.
DriverConfig driverConfigFromKVPs(std::map<std::string, std::string> kvps) {
  DriverConfig config;

  if (kvps.count("dsn")) {
    config.setDSN(kvps.at("dsn"));
  }
  if (kvps.count("driver")) {
    config.setDriver(kvps.at("driver"));
  }

  applyKVP(kvps, "hostname", [&](const std::string& v) {
    config.setHostname(v);
  });
  applyKVP(kvps, "port", [&](const std::string& v) { config.setPort(v); });
  applyKVP(kvps, "loglevel", [&](const std::string& v) {
    config.setLogLevel(v);
  });
  applyKVP(kvps, "authmethod", [&](const std::string& v) {
    config.setAuthMethod(v);
  });
  applyKVP(kvps, "oidcDiscoveryUrl", [&](const std::string& v) {
    config.setOidcDiscoveryUrl(v);
  });
  applyKVP(kvps, "clientId", [&](const std::string& v) {
    config.setClientId(v);
  });
  applyKVP(kvps, "clientSecret", [&](const std::string& v) {
    config.setClientSecret(v);
  });
  applyKVP(kvps, "oidcScope", [&](const std::string& v) {
    config.setOidcScope(v);
  });
  applyKVP(kvps, "grantType", [&](const std::string& v) {
    config.setGrantType(v);
  });
  applyKVP(kvps, "tokenEndpoint", [&](const std::string& v) {
    config.setTokenEndpoint(v);
  });

  /*
  Non-interactive credential options.

  The user name and password accept several spellings because different client
  ecosystems send different ones. Microsoft ODBC clients and the Power BI
  on-premises data gateway send UID= and PWD= in the connection string; the
  ClickHouse ODBC driver, which is the usual reference for Power BI ODBC
  setups, accepts both UID/Username and PWD/Password. Accepting all of them
  means a DSN copied from a ClickHouse working example behaves the same way
  here. Order matters: the last write wins, so the more specific spellings are
  applied after the generic ones.
  */
  applyKVP(kvps, "username", [&](const std::string& v) {
    config.setUsername(v);
  });
  applyKVP(kvps, "uid", [&](const std::string& v) { config.setUsername(v); });
  applyKVP(kvps, "password", [&](const std::string& v) {
    config.setPassword(v);
  });
  applyKVP(kvps, "pwd", [&](const std::string& v) { config.setPassword(v); });
  applyKVP(kvps, "token", [&](const std::string& v) { config.setToken(v); });
  applyKVP(kvps, "tokenFile", [&](const std::string& v) {
    config.setTokenFile(v);
  });
  applyKVP(kvps, "tokenCommand", [&](const std::string& v) {
    config.setTokenCommand(v);
  });
  applyKVP(kvps, "servicePrincipalName", [&](const std::string& v) {
    config.setServicePrincipalName(v);
  });

  // Transport options.
  applyKVP(kvps, "sslCAInfo", [&](const std::string& v) {
    config.setSslCAInfo(v);
  });
  applyKVP(kvps, "sslVerify", [&](const std::string& v) {
    config.setSslVerify(v);
  });
  applyKVP(kvps, "requestTimeoutMs", [&](const std::string& v) {
    config.setRequestTimeoutMs(v);
  });
  applyKVP(kvps, "clientInfo", [&](const std::string& v) {
    config.setClientInfo(v);
  });

  return config;
}

// Create key value pairs from a config.
std::map<std::string, std::string> driverConfigToKVPs(DriverConfig config) {
  std::map<std::string, std::string> kvps;

  /*
  dsn and driver identify the entry rather than configure it, so they are only
  emitted when set. Everything below is a setting and is written unconditionally,
  including empty values.

  Writing empty values matters: this function feeds ConfigDSN's loop that
  persists the whole form to ODBC.INI. If an empty setting were skipped, clearing
  a field in the dialog would leave the previous value in the profile, so
  deleting a password would leave the old (encrypted) password in place and still
  in use. It is also safe for the other caller, SQLDriverConnect, because
  driverConfigFromKVPs ignores empty values.
  */
  if (!config.getDSN().empty()) {
    kvps["dsn"] = config.getDSN();
  }
  if (!config.getDriver().empty()) {
    kvps["driver"] = config.getDriver();
  }

  kvps["hostname"]         = config.getHostname();
  kvps["port"]             = config.getPortStr();
  kvps["loglevel"]         = config.getLogLevelStr();
  kvps["authmethod"]       = config.getAuthMethodStr();
  kvps["oidcDiscoveryUrl"] = config.getOidcDiscoveryUrl();
  kvps["clientId"]         = config.getClientId();
  kvps["clientSecret"]     = config.getClientSecret();
  kvps["oidcScope"]        = config.getOidcScope();
  kvps["tokenEndpoint"]    = config.getTokenEndpoint();
  kvps["grantType"]        = config.getGrantType();

  kvps["username"]             = config.getUsername();
  kvps["password"]             = config.getPassword();
  kvps["token"]                = config.getToken();
  kvps["tokenFile"]            = config.getTokenFile();
  kvps["tokenCommand"]         = config.getTokenCommand();
  kvps["servicePrincipalName"] = config.getServicePrincipalName();

  kvps["sslCAInfo"]      = config.getSslCAInfo();
  kvps["sslVerify"]      = config.getSslVerify() ? "true" : "false";
  kvps["clientInfo"]     = config.getClientInfo();
  kvps["requestTimeoutMs"] = std::to_string(config.getRequestTimeoutMs());

  return kvps;
}
