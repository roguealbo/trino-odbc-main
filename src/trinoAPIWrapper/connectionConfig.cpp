#include "connectionConfig.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

#include "../util/callbackHelper.hpp"
#include "../util/stringTrim.hpp"
#include "../util/writeLog.hpp"
#include "authProvider/basicAuthProvider.hpp"
#include "authProvider/clientCredAuthProvider.hpp"
#include "authProvider/deviceFlowAuthProvider.hpp"
#include "authProvider/externalAuthProvider.hpp"
#include "authProvider/kerberosAuthProvider.hpp"
#include "authProvider/noAuthProvider.hpp"
#include "authProvider/tokenAuthProvider.hpp"


using json = nlohmann::json;

static size_t
curlWriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
  size_t totalSize = size * nmemb;
  s->append(static_cast<char*>(contents), totalSize);
  return totalSize;
}

static size_t
curlHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
  std::map<std::string, std::string>* responseHeaderData =
      (std::map<std::string, std::string>*)userdata;
  std::string headerData = std::string(buffer, nitems);
  if (headerData.starts_with("HTTP/")) {
    // The HTTP/<version> header is not a key value pair, so
    // it gets some special logic.
    responseHeaderData->insert({"http", headerData});
  } else {
    auto firstColon = headerData.find(':');
    if (firstColon != std::string::npos) {
      std::string key   = headerData.substr(0, firstColon);
      std::string value = headerData.substr(firstColon + 1);
      // Normalize header name to lowercase so lookups work regardless of
      // whether the server uses HTTP/1.1 title-case or HTTP/2 lowercase.
      std::transform(key.begin(), key.end(), key.begin(), ::tolower);
      // The values usually end in \r\n at a minimum, so we need
      // to trim that off.
      trim(value);
      auto [it, inserted] = responseHeaderData->insert({key, value});
      if (!inserted) {
        it->second += "\n" + value;
      }
    }
  }
  // Return the number of bytes consumed to signal success.
  return nitems * size;
}

/*
Build the User-Agent value. Trino's client protocol does not require it, but
when several reports share one gateway it is the only way to tell in the
coordinator's query log which DSN produced which query.
*/
static std::string buildUserAgent(const std::string& clientInfo) {
  std::string userAgent = "TrinoODBC";
  if (not clientInfo.empty()) {
    userAgent += " (" + clientInfo + ")";
  }
  return userAgent;
}

ConnectionConfig::ConnectionConfig(const ConnectionSetup& setup) {
  if (setup.hostname.empty()) {
    throw std::invalid_argument("hostname");
  }

  this->hostname       = setup.hostname;
  this->port           = setup.port;
  this->connectionName = setup.connectionName;
  this->authMethod     = setup.authMethod;
  this->tokenEndpoint  = setup.tokenEndpoint;
  this->grantType      = setup.grantType;
  this->options        = setup.options;
  this->configuredUser = setup.credentials.username;

  this->curl = nullptr;

  /*
  A gateway-hosted DSN cannot complete an interactive flow: the on-premises
  data gateway service has no desktop session, so opening a browser or waiting
  on a device-code prompt would hang until the request times out. Failing fast
  with a clear message is far better than that.
  */
  if (not isNonInteractiveAuthMethod(setup.authMethod) and
      setup.connectionName.empty()) {
    WriteLog(LL_WARN,
             "  Interactive authentication method selected without a DSN. "
             "Interactive flows require a desktop session and will fail under "
             "a service host such as the Power BI on-premises data gateway.");
  }

  switch (setup.authMethod) {
    case AM_NO_AUTH: {
      this->authConfigPtr = getNoAuthConfigPtr(
          this->hostname, this->port, this->connectionName, setup.credentials);
      break;
    }
    case AM_EXTERNAL_AUTH: {
      this->authConfigPtr = getExternalAuthConfigPtr(
          this->hostname, this->port, this->connectionName);
      break;
    }
    case AM_CLIENT_CRED_AUTH: {
      this->authConfigPtr = getClientCredAuthProvider(this->hostname,
                                                      this->port,
                                                      this->connectionName,
                                                      setup.oidcDiscoveryUrl,
                                                      setup.clientId,
                                                      setup.clientSecret,
                                                      setup.oidcScope,
                                                      setup.grantType,
                                                      setup.tokenEndpoint);
      break;
    }
    case AM_DEVICE_FLOW: {
      this->authConfigPtr = getDeviceFlowAuthProvider(this->hostname,
                                                      this->port,
                                                      this->connectionName,
                                                      setup.oidcDiscoveryUrl,
                                                      setup.clientId,
                                                      setup.clientSecret,
                                                      setup.oidcScope);
      break;
    }
    case AM_BASIC_AUTH: {
      this->authConfigPtr = getBasicAuthConfigPtr(this->hostname,
                                                  this->port,
                                                  this->connectionName,
                                                  setup.credentials.username,
                                                  setup.credentials.password);
      break;
    }
    case AM_TOKEN_AUTH: {
      this->authConfigPtr =
          getTokenAuthConfigPtr(this->hostname,
                                this->port,
                                this->connectionName,
                                setup.credentials.username,
                                setup.credentials.token,
                                setup.credentials.tokenFile,
                                setup.credentials.tokenCommand);
      break;
    }
    case AM_KERBEROS_AUTH: {
      this->authConfigPtr =
          getKerberosAuthConfigPtr(this->hostname,
                                   this->port,
                                   this->connectionName,
                                   setup.credentials.username,
                                   setup.credentials.servicePrincipalName);
      break;
    }

    default: {
      WriteLog(LL_ERROR,
               "  ERROR: connection config got unimplemented external auth "
               "method value: " +
                   std::to_string(setup.authMethod));
    }
  }
}

ConnectionConfig::~ConnectionConfig() {
  curl_easy_cleanup(this->curl);
}

std::string const ConnectionConfig::getHostname() {
  return this->hostname;
}

/*
Obtain the server name, which is essentially the "netloc"
component of the URL as far as I can tell from the docuentation.
*/
std::string const ConnectionConfig::getServerName() {
  size_t slashPos = this->hostname.find("//");
  if (slashPos == std::string::npos) {
    return this->hostname;
  } else {
    return this->hostname.substr(slashPos + 2);
  }
}

unsigned short const ConnectionConfig::getPort() {
  return this->port;
}

ApiAuthMethod const ConnectionConfig::getAuthMethod() {
  return this->authMethod;
}

std::string ConnectionConfig::getTrinoUser() {
  /*
  Prefer the identity the auth provider resolved, because that is the one the
  coordinator actually sees. Fall back to the configured user name for a
  provider that has not authenticated yet.

  There is deliberately no "TestUser" fallback here. The no-auth provider sets
  that itself, so reaching this code with nothing configured means External Auth
  or a provider that never resolved an identity; claiming to be TestUser in that
  case would be a lie that is hard to spot. An empty string is the honest answer
  and SQLGetInfo permits it.
  */
  if (this->authConfigPtr and not this->authConfigPtr->trinoUser.empty()) {
    return this->authConfigPtr->trinoUser;
  }
  return this->configuredUser;
}

std::string const ConnectionConfig::getStatementUrl() {
  return (this->hostname) + ":" + std::to_string(port) + "/v1/statement";
}

CURL* ConnectionConfig::getCurl() {
  /*
  Return a curl handle, reset it if needed, and it's ready to go.
  */
  if (this->curl == nullptr) {
    this->curl = curl_easy_init();
    // We always want to use SSL.
    curl_easy_setopt(this->curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
    // We want to save the response body in a string using a callback.
    curl_easy_setopt(this->curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(this->curl, CURLOPT_WRITEDATA, &(this->responseData));
    // We want to parse response headers.
    curl_easy_setopt(this->curl, CURLOPT_HEADERFUNCTION, curlHeaderCallback);
    curl_easy_setopt(
        this->curl, CURLOPT_HEADERDATA, &(this->responseHeaderData));
    // Set a timeout on all requests
    curl_easy_setopt(
        this->curl, CURLOPT_TIMEOUT_MS, this->options.requestTimeoutMs);
    // Enable gzip and/or deflate on responses
    curl_easy_setopt(this->curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");
    // Force HTTP/2 so response header names are always lowercase, which
    // keeps our case-normalized responseHeaderData map consistent.
    curl_easy_setopt(this->curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);

    /*
    Certificate verification. By default the OS trust store is used (via
    CURLSSLOPT_NATIVE_CA above). A DSN can point at an explicit CA bundle for
    a private CA, or disable verification entirely for a self-signed
    coordinator. Disabling verification is logged at WARN because it is a
    security downgrade that a gateway administrator should not do by accident.
    */
    if (not this->options.sslVerify) {
      WriteLog(LL_WARN,
               "  TLS certificate verification is DISABLED by DSN setting. "
               "Only do this for coordinators with self-signed certificates.");
      curl_easy_setopt(this->curl, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(this->curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (not this->options.sslCAInfo.empty()) {
      curl_easy_setopt(
          this->curl, CURLOPT_CAINFO, this->options.sslCAInfo.c_str());
    }

    // Identify the calling application to the coordinator.
    std::string userAgent = buildUserAgent(this->options.clientInfo);
    curl_easy_setopt(this->curl, CURLOPT_USERAGENT, userAgent.c_str());
  }

curlSetup:
  // Clear the previous response data, we do not want to append to it.
  this->responseData.clear();
  // Clear the previous response headers as well
  this->responseHeaderData.clear();

  // We could do a full reset here, but that seems to slow the driver
  // down considerably. Better to just reset a few things and
  // otherwise reuse the curl handle.
  // curl_easy_reset(this->curl);

  // Let's say the standard state of curl is that the
  // handle is configured to run GET requests, no matter
  // how it was used before.
  curl_easy_setopt(this->curl, CURLOPT_HTTPGET, true);

  // Set up any required headers if needed. We may need to handle
  // both auth headers and non-auth headers.
  struct curl_slist* headers = nullptr;
  for (const auto& pair : this->nonAuthHeaders) {
    std::string nextHeader = pair.first + ": " + pair.second;
    headers                = curl_slist_append(headers, nextHeader.c_str());
  }
  if (this->authConfigPtr->headers.size() > 0) {
    for (const auto& pair : this->authConfigPtr->headers) {
      std::string nextHeader = pair.first + ": " + pair.second;
      headers                = curl_slist_append(headers, nextHeader.c_str());
    }
  }
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  // Now that we have a fully configured CURL handle, check if we need to do
  // any required auth steps. We may need to use the configured handle to
  // perform the authentication.
  if (this->authConfigPtr->isExpired()) {
    WriteLog(LL_TRACE,
             "  Detected expired authentication. Reauthenticating...");
    this->authConfigPtr->refresh(
        this->curl, &(this->responseData), &(this->responseHeaderData));
    goto curlSetup;
  }

  return this->curl;
}

long ConnectionConfig::getLastHTTPStatusCode() {
  long httpStatusCode = -1;
  if (this->curl) {
    curl_easy_getinfo(this->curl, CURLINFO_RESPONSE_CODE, &httpStatusCode);
  }
  return httpStatusCode;
}

void ConnectionConfig::disconnect() {
  for (std::function f : this->onDisconnectCallbacks) {
    f(this);
  }
  if (this->curl) {
    curl_easy_cleanup(this->curl);
    this->curl = nullptr;
  }
}

std::string ConnectionConfig::getTrinoServerVersion() {
  CURL* curl = this->getCurl();

  std::string url =
      this->hostname + ":" + std::to_string(this->port) + "/v1/info";

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    WriteLog(LL_ERROR,
             "Failed to read trino server version: " +
                 std::string(curl_easy_strerror(res)));
    return "";
  }

  try {
    json jsonResponse = nlohmann::json::parse(this->responseData);
    return jsonResponse["nodeVersion"]["version"].get<std::string>();
  } catch (const std::exception& e) {
    /*
    /v1/info is served unauthenticated on some deployments and can return an
    HTML error page when a proxy sits in front of the coordinator. SQLGetInfo
    only uses this to decorate SQL_DBMS_VER, so a parse failure should degrade
    to an empty version rather than throwing during connection setup.
    */
    WriteLog(LL_WARN,
             "  Could not parse server version response: " +
                 std::string(e.what()));
    return "";
  }
}

void ConnectionConfig::registerDisconnectCallback(
    std::function<void(ConnectionConfig*)> f) {
  this->onDisconnectCallbacks.push_back(f);
}

void ConnectionConfig::unregisterDisconnectCallback(
    std::function<void(ConnectionConfig*)> f) {
  removeCallbackFromVector(this->onDisconnectCallbacks, f);
}

void ConnectionConfig::setRequestHeader(std::string key, std::string value) {
  // Specifically must use [] and not map.insert, since the latter will not
  // overwrite existing keys.
  this->nonAuthHeaders[key] = value;
}

std::string ConnectionConfig::getRequestHeader(std::string key) {
  if (this->nonAuthHeaders.count(key) > 0) {
    return this->nonAuthHeaders[key];
  } else {
    return "";
  }
}

void ConnectionConfig::clearRequestHeader(std::string key) {
  this->nonAuthHeaders.erase(key);
}
