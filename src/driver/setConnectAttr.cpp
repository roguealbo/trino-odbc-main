#include "../util/windowsLean.hpp"
#include <sql.h>
#include <sqlext.h>

#include "../util/stringFromChar.hpp"
#include "../util/writeLog.hpp"
#include "handles/connHandle.hpp"

/*
SQL_ATTR_UID and SQL_ATTR_PASSWORD were introduced in ODBC 3.8 and are absent
from older Windows SDK headers. The driver advertises DriverODBCVer 03.80, so
it should accept them, but defining the values locally keeps the build working
against any SDK. The numeric values are fixed by the ODBC specification.
*/
#ifndef SQL_ATTR_UID
#define SQL_ATTR_UID 60
#endif
#ifndef SQL_ATTR_PASSWORD
#define SQL_ATTR_PASSWORD 61
#endif

SQLRETURN SQL_API SQLSetConnectOption(SQLHDBC ConnectionHandle,
                                      SQLUSMALLINT Option,
                                      SQLULEN Value) {
  // A deprecated ODBC2.0 version
  WriteLog(LL_WARN, "Entering SQLSetConnectOption (deprecated)");
  WriteLog(LL_WARN, "  Setting option: " + std::to_string(Option));
  WriteLog(LL_WARN, "  To value: " + std::to_string(Value));

  WriteLog(LL_ERROR, "  ERROR - Returning error from SQLSetConnectOption");
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLSetConnectAttr(SQLHDBC ConnectionHandle,
                                    SQLINTEGER Attribute,
                                    _In_reads_bytes_opt_(StringLength)
                                        SQLPOINTER Value,
                                    SQLINTEGER StringLength) {
  /*
  The Value parameter claims to be a SQLPOINTER, but it is only a pointer
  in the event that the attribute that is being set is a string type attribute.
  When setting integer type attributes such as SQL_ATTR_AUTOCOMMIT, SQLPOINTER
  is not a pointer, it's a 32 bit integer that is passed by value.
  */

  WriteLog(LL_TRACE, "Entering SQLSetConnectAttr");
  Connection* connection = reinterpret_cast<Connection*>(ConnectionHandle);
  WriteLog(LL_TRACE,
           "  Request to set attribute: " + std::to_string(Attribute));

  if (connection == nullptr) {
    WriteLog(LL_ERROR, "  ERROR: ConnectionHandle is invalid.");
    return SQL_ERROR;
  }

  switch (Attribute) {
    case SQL_ATTR_AUTOCOMMIT: { // 102
      SQLINTEGER autocommitMode =
          static_cast<SQLINTEGER>(reinterpret_cast<std::uintptr_t>(Value));
      connection->ATTR_AutoCommitMode = autocommitMode;
      WriteLog(LL_TRACE,
               "  Autocommit mode set to: " + std::to_string(autocommitMode));
      break;
    }
    case SQL_ATTR_LOGIN_TIMEOUT: { // 103
      SQLUINTEGER loginTimeout =
          static_cast<SQLUINTEGER>(reinterpret_cast<std::uintptr_t>(Value));
      connection->ATTR_LoginTimeout = loginTimeout;
      WriteLog(LL_TRACE,
               "  Login timeout set to: " + std::to_string(loginTimeout));
      break;
    }
    case SQL_ATTR_UID: { // 60
      /*
      Clients such as the Power BI on-premises data gateway supply credentials
      this way rather than embedding them in the DSN. The value is a
      null-terminated string pointer, unlike the integer attributes above.
      */
      if (Value == nullptr) {
        WriteLog(LL_ERROR, "  ERROR: SQL_ATTR_UID value pointer is null.");
        return SQL_ERROR;
      }
      connection->ATTR_UserName =
          stringFromChar(reinterpret_cast<SQLCHAR*>(Value), SQL_NTS);
      connection->ATTR_CredentialsProvided = true;
      WriteLog(LL_TRACE, "  Connection user name set via SQLSetConnectAttr.");
      break;
    }
    case SQL_ATTR_PASSWORD: { // 61
      if (Value == nullptr) {
        WriteLog(LL_ERROR, "  ERROR: SQL_ATTR_PASSWORD value pointer is null.");
        return SQL_ERROR;
      }
      connection->ATTR_Password =
          stringFromChar(reinterpret_cast<SQLCHAR*>(Value), SQL_NTS);
      connection->ATTR_CredentialsProvided = true;
      WriteLog(LL_TRACE,
               "  Connection password set via SQLSetConnectAttr.");
      break;
    }
    default: {
      WriteLog(LL_ERROR, "  ERROR: Unsupported attribute in SetConnectAttr.");
      WriteLog(LL_ERROR, "  Attribute is " + std::to_string(Attribute));
      return SQL_ERROR;
    }
  }
  WriteLog(LL_TRACE, "  Successful connect attribute setting.");
  return SQL_SUCCESS;
}
