#include "../../util/windowsLean.hpp"
#include <sql.h>
#include <sqlext.h>

#include <algorithm>
#include <functional>
#include <map>
#include <odbcinst.h>
#include <string>
#include <vector>

#include "../../util/cryptUtils.hpp"
#include "../../util/delimKvpHelper.hpp"
#include "../../util/writeLog.hpp"
#include "dsnConfigForm.hpp"
#include "profileReader.hpp"

bool isSystemDSN() {
  /*
  System DSN secrets need to be decryptable for anyone on the system.
  User DSN secrets only need to be decryptable for the current user.
  To tell them apart, we need to use SQLGetConfigMode.

  There exists a ODBC_BOTH_DSN option, which we will treat the same
  as a system DSN in terms of encryption/decryption of secrets.
  */
  UWORD configMode;
  SQLGetConfigMode(&configMode);

  bool isSystemDSN = configMode != ODBC_USER_DSN;
  WriteLog(LL_INFO, "System DSN: " + std::to_string(isSystemDSN));
  return isSystemDSN;
}

/*
DSN keys whose values must never be written to ODBC.INI in the clear. They are
DPAPI-encrypted and stored under an "encrypted<Name>" key, mirroring how the
client secret has always been handled.
*/
static const std::vector<std::string> SECRET_KEYS = {
    "clientSecret", "password", "token"};

static bool isSecretKey(const std::string& section) {
  return std::find(SECRET_KEYS.begin(), SECRET_KEYS.end(), section) !=
         SECRET_KEYS.end();
}

void writeToProfile(DriverConfig result,
                    std::string section,
                    std::string value) {
  std::string dsn = result.getDSN();
  if (isSecretKey(section)) {
    std::function<std::string(const std::string&)> encryptionFunc = nullptr;
    std::string encryptionLevel;
    if (isSystemDSN()) {
      encryptionFunc  = systemEncryptString;
      encryptionLevel = "system";
    } else {
      encryptionFunc  = userEncryptString;
      encryptionLevel = "user";
    }

    // The stored key is "encrypted" plus the original key name with its first
    // letter capitalised: clientSecret -> encryptedClientSecret.
    std::string storedKey =
        "encrypted" + section.substr(0, 1) + section.substr(1);

    SQLWritePrivateProfileString(dsn.c_str(),
                                 storedKey.c_str(),
                                 encryptionFunc(value).c_str(),
                                 "ODBC.INI");
    SQLWritePrivateProfileString(dsn.c_str(),
                                 "secretEncryptionLevel",
                                 encryptionLevel.c_str(),
                                 "ODBC.INI");

  } else {
    SQLWritePrivateProfileString(
        dsn.c_str(), section.c_str(), value.c_str(), "ODBC.INI");
  }
}

BOOL APIENTRY ConfigDSN(HWND hwndParent,
                        WORD fRequest,
                        LPCSTR lpszDriver,
                        LPCSTR lpszAttributes) {

  WriteLog(LL_INFO, "Entering ConfigDSN");

  std::map<std::string, std::string> attributes =
      parseKVPsFromNullDelimChars(lpszAttributes);

  WriteLog(LL_TRACE, "  lpszAttributes are: ");
  WriteLog(LL_TRACE, attributes);

  DSNForm form = DSNForm(hwndParent, attributes);

  switch (fRequest) {
    case ODBC_ADD_DSN: {
      WriteLog(LL_TRACE, "  Request to add a DSN");
      form.ShowDSNForm();
      SQLWriteDSNToIni(form.getResult().getDSN().c_str(), lpszDriver);
      break;
    }
    case ODBC_CONFIG_DSN: {
      WriteLog(LL_TRACE, "  Request to configure a DSN");
      // For a config operation, we already have a DSN configured.
      // We want to load that profile and populate the form from
      // the values from that prior configuration.
      try {
        std::string dsn     = attributes.at("DSN");
        DriverConfig config = readDriverConfigFromProfile(dsn);
        form.setReadOnlyDSN();
        form.updateConfig(config);
      } catch (const std::exception& e) {
        /*
        Reading the profile decrypts the stored secrets, which fails when the
        DSN was created by a different user or ODBC.INI was edited by hand.
        Show an empty form rather than throwing into the driver manager; the
        administrator can re-enter the values.
        */
        WriteLog(LL_ERROR, "  ERROR: could not read existing DSN profile: " +
                               std::string(e.what()));
      }
      form.ShowDSNForm();
      break;
    }
    case ODBC_REMOVE_DSN: {
      WriteLog(LL_TRACE, "  Request to remove a DSN");
      SQLRemoveDSNFromIni(form.getResult().getDSN().c_str());
      break;
    }
    default: {
      WriteLog(LL_WARN, "  Unknown request");
      return false;
      break;
    }
  }

  WriteLog(LL_TRACE, "  Grabbing form result");
  DriverConfig result = form.getResult();

  if (result.getIsSaved() and SQLValidDSN(result.getDSN().c_str())) {
    /*
    Encrypting and writing the profile can throw: DPAPI fails if the caller
    cannot access the machine or user key, and reading an existing profile for
    the CONFIG case fails if a stored secret will not decrypt. ConfigDSN is
    called by the ODBC driver manager, so an exception escaping here unwinds
    through it and terminates whatever tool is configuring the DSN. Report the
    failure as a declined configuration instead.
    */
    try {
      std::map<std::string, std::string> kvps = driverConfigToKVPs(result);

      // Just write every attribute of the driver config to the profile.
      for (auto it = kvps.begin(); it != kvps.end(); it++) {
        writeToProfile(result, it->first, it->second);
      }
    } catch (const std::exception& e) {
      WriteLog(LL_ERROR,
               "  ERROR: failed to save DSN: " + std::string(e.what()));
      return false;
    }
    return true;
  } else {
    return false;
  }
}
