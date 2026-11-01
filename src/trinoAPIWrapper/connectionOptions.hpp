#pragma once

#include <string>

/*
Transport and client-identification options shared by every authentication
method.

These are modelled on the parameter set that the ClickHouse ODBC driver exposes
(SSLMode / CALocation / Timeout / ClientName), because that set is what makes
that driver practical to run inside the Microsoft Power BI on-premises data
gateway: a gateway-hosted DSN cannot rely on interactive prompts, so every
knob that affects connectivity has to be reachable from the DSN or connection
string.
*/
struct ConnectionOptions {
  /*
  Path to a CA bundle file or directory used to verify the coordinator's TLS
  certificate. Empty means "use the operating system trust store", which is
  what CURLSSLOPT_NATIVE_CA does on Windows. Set this when Trino is fronted by
  a private or enterprise CA that the gateway machine does not already trust.
  */
  std::string sslCAInfo = "";

  /*
  When false, peer certificate verification is disabled. This exists for
  gateways that must reach a coordinator with a self-signed certificate, and it
  is logged loudly because it weakens the connection.
  */
  bool sslVerify = true;

  /*
  Per-request timeout in milliseconds. The historical driver default was a
  fixed 10 seconds, which is too short for the large result-set polls that
  Power BI imports generate.
  */
  long requestTimeoutMs = 10000;

  /*
  Free-form text identifying the calling application. Sent as X-Trino-Source
  and as part of the User-Agent so that Trino's query logs and event listeners
  can attribute load to a specific gateway or report.
  */
  std::string clientInfo = "";
};
