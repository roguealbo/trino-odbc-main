#include "profileReader.hpp"

#include "../../util/windowsLean.hpp"
#include <sql.h>
#include <sqlext.h>

#include <cstdint>
#include <odbcinst.h>
#include <stdexcept>
#include <string>

#include "../../util/cryptUtils.hpp"
#include "../../util/writeLog.hpp"
#include "driverConfig.hpp"


std::string readFromPrivateProfile(std::string dsn, std::string key) {
  // Big buffer to hold values pulled out of the profile.
  char value[2048];

  std::string defaultVal = "";
  if (DRIVER_CONFIG_DEFAULT_VALUES.count(key)) {
    defaultVal = DRIVER_CONFIG_DEFAULT_VALUES.at(key);
  }

  SQLGetPrivateProfileString(dsn.c_str(),
                             key.c_str(),
                             defaultVal.c_str(),
                             value,
                             sizeof(value),
                             "ODBC.INI");

  return std::string(value);
}

/*
Decrypt a secret that was stored by writeToProfile in configDSN.cpp.

An empty ciphertext decrypts to an empty string, which is the correct
behaviour for DSNs that do not use the secret (for example a Basic-auth DSN
has no encryptedClientSecret, and an OIDC DSN has no encryptedPassword).
*/
static std::string decryptSecret(const std::string& encrypted,
                                 const std::string& encryptionLevel,
                                 const std::string& secretName) {
  if (encrypted.empty()) {
    return "";
  }
  if (encryptionLevel == "user") {
    return userDecryptString(encrypted);
  } else if (encryptionLevel == "system") {
    return systemDecryptString(encrypted);
  } else {
    throw std::runtime_error("Unknown secret encryption level: " +
                             encryptionLevel + " (for " + secretName + ")");
  }
}

DriverConfig readDriverConfigFromProfile(std::string dsn) {
  // The driver config reads values assuming mostly camelCase names.
  DriverConfig config;
  config.setDSN(dsn);
  config.setHostname(readFromPrivateProfile(dsn, "hostname"));
  config.setPort(readFromPrivateProfile(dsn, "port"));
  config.setLogLevel(readFromPrivateProfile(dsn, "loglevel"));
  config.setAuthMethod(readFromPrivateProfile(dsn, "authmethod"));
  config.setOidcDiscoveryUrl(readFromPrivateProfile(dsn, "oidcDiscoveryUrl"));
  config.setClientId(readFromPrivateProfile(dsn, "clientId"));
  config.setOidcScope(readFromPrivateProfile(dsn, "oidcScope"));
  config.setTokenEndpoint(readFromPrivateProfile(dsn, "tokenEndpoint"));
  config.setGrantType(readFromPrivateProfile(dsn, "grantType"));

  // Non-interactive credential options. The username is stored in the clear
  // (it is sent as X-Trino-User anyway); the password and JWT are DPAPI
  // encrypted like the client secret.
  config.setUsername(readFromPrivateProfile(dsn, "username"));
  config.setTokenFile(readFromPrivateProfile(dsn, "tokenFile"));
  config.setTokenCommand(readFromPrivateProfile(dsn, "tokenCommand"));
  config.setServicePrincipalName(
      readFromPrivateProfile(dsn, "servicePrincipalName"));

  // Transport options.
  config.setSslCAInfo(readFromPrivateProfile(dsn, "sslCAInfo"));
  config.setSslVerify(readFromPrivateProfile(dsn, "sslVerify"));
  config.setRequestTimeoutMs(readFromPrivateProfile(dsn, "requestTimeoutMs"));
  config.setClientInfo(readFromPrivateProfile(dsn, "clientInfo"));

  std::string secretEncryptionLevel =
      readFromPrivateProfile(dsn, "secretEncryptionLevel");
  std::string encryptedClientSecret =
      readFromPrivateProfile(dsn, "encryptedClientSecret");
  std::string encryptedPassword =
      readFromPrivateProfile(dsn, "encryptedPassword");
  std::string encryptedToken = readFromPrivateProfile(dsn, "encryptedToken");

  // Handle the encrypted data
  config.setClientSecret(decryptSecret(
      encryptedClientSecret, secretEncryptionLevel, "clientSecret"));
  config.setPassword(
      decryptSecret(encryptedPassword, secretEncryptionLevel, "password"));
  config.setToken(
      decryptSecret(encryptedToken, secretEncryptionLevel, "token"));

  return config;
}
