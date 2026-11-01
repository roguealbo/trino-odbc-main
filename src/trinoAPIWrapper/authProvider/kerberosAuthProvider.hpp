#pragma once

#include <memory>
#include <string>

#include "authConfig.hpp"

/*
Kerberos / Negotiate authentication via Windows SSPI.

This mirrors how the commercial Trino ODBC drivers implement Kerberos: the
driver asks the operating system for a service ticket for the Trino service
principal and presents it as an HTTP Negotiate token. Because the ticket comes
from the machine's credential cache, no browser or prompt is involved, so this
works under the Power BI on-premises data gateway service account provided that
account is domain-joined and the Trino service principal is registered to it.

servicePrincipalName should be of the form "trino/<fqdn>" (the Kerberos
service principal registered for the coordinator). If it is left empty the
driver derives "HTTP/<hostname>".
*/
std::unique_ptr<AuthConfig> getKerberosAuthConfigPtr(std::string hostname,
                                                     unsigned short port,
                                                     std::string connectionName,
                                                     std::string username,
                                                     std::string servicePrincipalName);
