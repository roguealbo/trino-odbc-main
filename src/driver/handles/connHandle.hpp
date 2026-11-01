#pragma once

#include "../../util/windowsLean.hpp"
#include <sql.h>
#include <sqlext.h>
#include <vector>

#include "../../trinoAPIWrapper/connectionConfig.hpp"
#include "../config/driverConfig.hpp"
#include "handleErrorInfo.hpp"
#include "statementHandle.hpp"

class Connection {
  private:
    EnvironmentConfig* environmentConfig = nullptr;
    ErrorInfo errorInfo;

  public:
    Connection(EnvironmentConfig* environmentConfig);
    ~Connection();
    void configure(DriverConfig config);
    bool connected                     = false;
    ConnectionConfig* connectionConfig = nullptr;
    void disconnect();

    SQLINTEGER ATTR_AutoCommitMode = SQL_AUTOCOMMIT_ON;
    SQLUINTEGER ATTR_LoginTimeout  = 0;

    /*
    Credentials supplied via SQLSetConnectAttr before the connection is made.
    Clients such as the Power BI on-premises data gateway set these rather than
    putting secrets in the DSN. They are applied over the DSN values when
    configure() runs.
    */
    std::string ATTR_UserName     = "";
    std::string ATTR_Password     = "";
    bool ATTR_CredentialsProvided = false;

    std::string getServerVersion();
    /*
    The identity the connection authenticated as, for SQLGetInfo(SQL_USER_NAME).
    Returns an empty string when no connection has been configured yet, which
    the ODBC spec permits before connect.
    */
    std::string getAuthenticatedUser();
    void setError(ErrorInfo errorInfo);
    ErrorInfo getError();

    void checkInputs(DriverConfig config);
};
