#include "../util/windowsLean.hpp"
#include <sql.h>
#include <sqlext.h>

#include <string>

#include "config/profileReader.hpp"
#include "handles/connHandle.hpp"

#include "../util/stringFromChar.hpp"
#include "../util/writeLog.hpp"


SQLRETURN SQL_API SQLConnect(SQLHDBC ConnectionHandle,
                             _In_reads_(NameLength1) SQLCHAR* DSNChars,
                             SQLSMALLINT NameLength1,
                             _In_reads_(NameLength2) SQLCHAR* UserNameChars,
                             SQLSMALLINT NameLength2,
                             _In_reads_(NameLength3)
                                 SQLCHAR* AuthenticationChars,
                             SQLSMALLINT NameLength3) {
  WriteLog(LL_TRACE, "Entering SQLConnect");
  std::string dsn        = stringFromChar(DSNChars, NameLength1);
  Connection* connection = reinterpret_cast<Connection*>(ConnectionHandle);

  /*
  Report failures through the standard ODBC diagnostic mechanism instead of
  letting an exception escape across the C ABI, which would terminate the host
  process. Power BI surfaces SQL_ERROR plus SQLGetDiagRec text to the user, so
  an actionable message here is what the administrator actually sees in the
  gateway's error pane.

  Note that reading the profile is inside the try block. It can throw: the
  secret decryption fails if ODBC.INI was edited by hand, and an unrecognised
  authmethod is reported rather than silently defaulting.
  */
  try {
    DriverConfig config = readDriverConfigFromProfile(dsn);

    /*
    Honour the credentials the application passes directly to SQLConnect.

    Microsoft clients (including the Power BI on-premises data gateway)
    commonly supply the user name and password here rather than embedding them
    in the DSN, and the ODBC specification says these arguments override the
    stored DSN values. An empty argument means "not supplied" and must leave
    the DSN value untouched rather than clearing it.

    They are routed through the same connection-level attributes that
    SQLSetConnectAttr uses, so there is a single precedence path in
    Connection::configure(). Arguments given directly to SQLConnect are the
    most specific source and therefore overwrite anything set earlier.
    */
    std::string suppliedUser = stringFromChar(UserNameChars, NameLength2);
    if (not suppliedUser.empty()) {
      WriteLog(LL_INFO, "  SQLConnect supplied a user name; overriding DSN");
      connection->ATTR_UserName            = suppliedUser;
      connection->ATTR_CredentialsProvided = true;
    }

    std::string suppliedAuth =
        stringFromChar(AuthenticationChars, NameLength3);
    if (not suppliedAuth.empty()) {
      WriteLog(LL_INFO,
               "  SQLConnect supplied authentication data; overriding DSN");
      connection->ATTR_Password            = suppliedAuth;
      connection->ATTR_CredentialsProvided = true;
    }

    // It's kind of unfortunate that we can't set the log level of the driver
    // until we've read the DSN in some way.
    WriteLog(LL_TRACE, "  Setting Log Level");
    setLogLevel(config.getLogLevelEnum());

    WriteLog(LL_TRACE, "  Configuring connection");
    connection->configure(config);
    connection->connected = true;
  } catch (std::exception& e) {
    WriteLog(LL_DEBUG, "  Error from SQLConnect: " + std::string(e.what()));
    ErrorInfo error = ErrorInfo(e.what(), "HY000");
    connection->setError(error);
    return SQL_ERROR;
  }

  return SQL_SUCCESS;
}
