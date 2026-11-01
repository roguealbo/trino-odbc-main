#include "../util/windowsLean.hpp"
#include <sql.h>
#include <sqlext.h>

#include "../util/valuePtrHelper.hpp"
#include "../util/writeLog.hpp"
#include "handles/connHandle.hpp"

/*
Match the definitions used by SQLSetConnectAttr so that a client that set its
credentials with SQL_ATTR_UID can read the effective identity back. Older
Windows SDK headers do not define these ODBC 3.8 attributes.
*/
#ifndef SQL_ATTR_UID
#define SQL_ATTR_UID 60
#endif

SQLRETURN SQL_API SQLGetConnectAttr(
    SQLHDBC ConnectionHandle,
    SQLINTEGER Attribute,
    _Out_writes_opt_(_Inexpressible_(BufferLength)) SQLPOINTER Value,
    SQLINTEGER BufferLength,
    _Out_opt_ SQLINTEGER* StringLengthPtr) {
  WriteLog(LL_TRACE, "Entering SQLGetConnectAttr");
  WriteLog(LL_TRACE,
           "  Application is requesting connection attribute: " +
               std::to_string(Attribute));
  Connection* connection = reinterpret_cast<Connection*>(ConnectionHandle);
  switch (Attribute) {
    case (SQL_ATTR_CONNECTION_DEAD): {
      // Deliberately empty: falls through to CURRENT_CATALOG below, which is
      // how this driver has always answered CONNECTION_DEAD. Any new case must
      // therefore be added after it, not between the two.
    }
    case (SQL_ATTR_CURRENT_CATALOG): {
      // Return an empty string to show that no catalog is assigned.
      // I'm not sure if you can set a specific catalog at the connection level.
      writeNullTermStringToPtr(Value, "system", StringLengthPtr);
      break;
    }
    case SQL_ATTR_UID: {
      /*
      Report the identity the connection actually carries rather than whatever
      was set via SQLSetConnectAttr, so a client that set no UID still learns who
      it is talking to. The password is deliberately not readable back: ODBC
      drivers are expected to accept a password and forget it.
      */
      writeNullTermStringToPtr(
          Value, connection->getAuthenticatedUser(), StringLengthPtr);
      break;
    }
    default: {
      WriteLog(LL_ERROR,
               "  ERROR: Application is requesting unimplemented connection "
               "attribute: " +
                   std::to_string(Attribute));
      return SQL_ERROR;
    }
  }
  return SQL_SUCCESS;
}
