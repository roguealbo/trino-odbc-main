#include "connHandle.hpp"

#include "../../util/hostEnvironment.hpp"

Connection::Connection(EnvironmentConfig* environmentConfig) {
  this->environmentConfig = environmentConfig;
}

Connection::~Connection() {
  if (this->connectionConfig) {
    delete this->connectionConfig;
  }
}

void Connection::disconnect() {
  /*
  A host application calls SQLDisconnect even when the connection failed, so
  this must tolerate a null ConnectionConfig. Before this, a failed SQLConnect
  followed by the client's usual cleanup would dereference null and crash the
  host instead of simply reporting that the connection is gone.
  */
  if (this->connectionConfig == nullptr) {
    return;
  }
  this->connectionConfig->disconnect();
  this->connected = false;
}

std::string Connection::getServerVersion() {
  if (this->connectionConfig == nullptr) {
    return "";
  }
  return this->connectionConfig->getTrinoServerVersion();
}

std::string Connection::getAuthenticatedUser() {
  /*
  Before configure() has run there is no identity to report. Returning empty
  rather than dereferencing a null ConnectionConfig keeps SQLGetInfo callable
  from clients that probe the connection handle early.
  */
  if (this->connectionConfig == nullptr) {
    return "";
  }
  return this->connectionConfig->getTrinoUser();
}

void Connection::checkInputs(DriverConfig config) {
  if (config.getHostname().empty()) {
    throw std::invalid_argument("hostname cannot be empty");
  } else if (config.getPortNum() <= 0) {
    throw std::invalid_argument("port must be greater than zero");
  }

  /*
  Fail fast on configurations that can never work in the host process. The
  Power BI on-premises data gateway runs as a Windows service with no desktop
  session, so an interactive auth method (browser-based External Auth or
  Device Flow) would hang until timeout. Catching this at connect time gives
  the administrator an actionable error instead.
  */
  if (not isNonInteractiveAuthMethod(config.getAuthMethodEnum()) and
      not isInteractiveDesktopAvailable()) {
    throw std::invalid_argument(
        "authentication method '" + config.getAuthMethodStr() +
        "' requires an interactive desktop session and cannot be used from a "
        "service host such as the Power BI on-premises data gateway. Use "
        "Basic Auth, Token Auth, Kerberos Auth, or OIDC Client Cred Auth "
        "instead.");
  }
}

void Connection::configure(DriverConfig config) {
  // This instantiates a driver config object on the heap.
  // The destructor will clean it up if that's happened.
  checkInputs(config);

  /*
  Credentials supplied through SQLSetConnectAttr or SQLConnect take precedence
  over the values stored in the DSN. That is the ODBC convention, and it is how
  the Power BI on-premises data gateway passes per-datasource credentials
  without them ever being written to ODBC.INI.
  */
  if (this->ATTR_CredentialsProvided) {
    if (not this->ATTR_UserName.empty()) {
      config.setUsername(this->ATTR_UserName);
    }
    if (not this->ATTR_Password.empty()) {
      config.setPassword(this->ATTR_Password);
    }
  }

  ConnectionSetup setup;
  setup.hostname           = config.getHostname();
  setup.port               = config.getPortNum();
  setup.connectionName     = config.getDSN();
  setup.authMethod         = config.getAuthMethodEnum();
  setup.oidcDiscoveryUrl   = config.getOidcDiscoveryUrl();
  setup.clientId           = config.getClientId();
  setup.clientSecret       = config.getClientSecret();
  setup.oidcScope          = config.getOidcScope();
  setup.grantType          = config.getGrantType();
  setup.tokenEndpoint      = config.getTokenEndpoint();
  setup.credentials        = config.getCredentials();
  setup.options            = config.getOptions();

  this->connectionConfig = new ConnectionConfig(setup);
}

void Connection::setError(ErrorInfo errorInfo) {
  this->errorInfo = errorInfo;
}

ErrorInfo Connection::getError() {
  return this->errorInfo;
}
