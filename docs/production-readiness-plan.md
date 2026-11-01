# Plan: make trino-odbc production-ready (ship-blockers + high-value)

## Context

`ROADMAP.md`, `notes.md`, and `AGENTS.md` describe a Windows/MSVC ODBC driver that has been extended for Power BI gateway use but **has never been compiled**. Two audits of the code found it is **not production-ready**. Several defects can crash or hang the host process (Excel/Power BI):

- Infinite `getCurl()` refresh loops in all OIDC providers (no guard flag; the 10-minute grace period makes 5-minute tokens look "expired" forever).
- Exceptions crossing the C ABI from `SQLFetch`, `SQLGetData`, `SQLTables`, `SQLColumns`, `SQLDescribeCol`, `SQLFreeHandle`/`SQLCancel` (via `terminate()`), and `SQLGetInfo`.
- `CURLOPT_CUSTOMREQUEST "DELETE"` is never cleared after cancel/terminate, so every later request on that connection is a DELETE.
- `poll()` ignores the HTTP status and retries forever when curl fails.
- Memory corruption: `SQLGetStmtAttr` memset with a negative length, `SQL_ATTR_CONNECTION_DEAD` writing 7 bytes into a 4-byte field, and `getInfo` string writes ignoring `BufferLength`.
- `SQLGetDiagRec` never returns statement errors. A failed `SQLPrepare` reports success and then runs the previous statement.

**Primary target (user requirement):** the driver must work unattended from an **M365 Power BI on-premises data gateway**, authenticating with either **OIDC** (Entra ID client credentials) or **LDAP** (Trino's password authenticator via Basic Auth). Phase 6b covers this. Agreed scope: the AGENTS.md "must do before shipping" and "high value" items, the Proxy parameter, structured error mapping, ROADMAP §5 phases 1–2 (backoff and Retry-After), and finishing prepared statements. Out of scope: metadata caching, spooling, OIDC discovery/refresh/STS/RAR, and mTLS.

**Execution order:** D (repo docs) → 0 → 1 → 2 → P2/P5/P6 → S1–S3 (critical security) → P1/P3/P4 → 6b → 3 → 4 → S4–S11 → 5 → 6 → 7.

All C++ must follow the AGENTS.md/CONTRIBUTING.md conventions: no member initializer lists, braces everywhere, `this->`, no typedefs, and minimal `const`/`static`/references except where needed. Comments explain *why*.

---

## Phase 0 — Compile-check harness (this machine)

- `brew install mingw-w64 nlohmann-json curl googletest` (user approved).
- Write a local script, `tools/syntax_check.sh`. It runs `x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only` over every `src/**/*.cpp`, with nlohmann/curl include paths and a small shim header for MSVC-only SAL macros if mingw lacks any.
- Pure-logic unit tests (described below) compile and run natively with Linux `g++` + gtest.
- This is not an MSVC build. Phase 6 adds a CI build for that.

## Phase 1 — Compile fixes

- `kerberosAuthProvider.cpp`: `#define SECURITY_WIN32` before `<sspi.h>`.
- Add the missing includes: `<format>` (execute.cpp, prepare.cpp, setStmtAttr.cpp, trinoQuery.cpp), `<sstream>` (trinoQuery, clientCred, deviceFlow), `<optional>` (tokenCacheAuthProviderBase.hpp), `<cstdlib>` (deviceFlow).
- Fix filename case in `CMakeLists.txt` (`trinoOdbcErrorHandler.cpp`, `delimKvpHelper.cpp`, `fetchGetdataTest.cpp`) and in the `#include "TrinoOdbcErrorHandler.hpp"` sites.
- `windowsLean.hpp`: wrap the macros in `#ifndef`.
- Move `ClientCredAuthParams` into one shared header. Fix deviceFlow including the wrong header.
- Fix whatever else the Phase 0 syntax check reports.

## Phase 2 — Crash and hang fixes (ship-blockers)

**Auth loop and throw safety** (`src/trinoAPIWrapper/authProvider/*`)
- `tokenCacheAuthProviderBase.cpp`:
  - Add a `refreshAttempted` guard, following the `TokenAuthConfig` pattern.
  - Wrap `refresh()` in `try { … } catch (...)` and log the error.
  - Never cache an empty token.
  - `isExpired()` returns false once a refresh has been attempted and the token is still not usable.
  - Add a guard-free `tokenIsExpired()`.
- `tokenCache.cpp`: grace period = min(600s, 10% of token lifetime). A token with no `exp` claim is treated as non-expiring, not as `exp=0`.
- `tokenParser.cpp`: bounds-check the JWT segments. An opaque token returns an empty claims object instead of indexing an empty vector.
- `tokenAuthProvider.cpp`: move `setToken()` inside the try.
- `applyUserHeader`: use `is_string()` checks.
- clientCred / external / deviceFlow:
  - Check the curl result and HTTP status.
  - Use `json::parse(..., nullptr, false)` with discard checks, and `.contains()` before reading fields.
  - deviceFlow: default `interval` to 5.
  - clientCred: actually send `scope`, and free the header list.
- deviceFlow: replace `std::system("start " + uri)` with the existing `browserInteraction` helper (ShellExecute), which removes the shell injection.
- `ConnectionConfig` ctor `default:` case throws `invalid_argument`, which SQLConnect/SQLDriverConnect already catch, instead of leaving `authConfigPtr` null.
- `tokenCache.cpp` read: catch `json::parse_error` and treat a corrupt cache file as empty.

**HTTP handle hygiene** (`connectionConfig.cpp`, `trinoQuery.cpp`)
- `getCurl()`:
  - Always reset `CURLOPT_CUSTOMREQUEST` to NULL and `CURLOPT_POSTFIELDS`.
  - Keep the `curl_slist` as a member and free it before rebuilding, which fixes the per-call leak.
- `terminate()` / `cancel()`: clear CUSTOMREQUEST after use, and log failures instead of throwing.
- `SQLCancel` called from another thread: guard the shared curl handle with a per-connection `std::mutex`. `SQLCancel` then uses a **separate** short-lived curl handle for the DELETE, so it never touches the handle that `poll()` is using.
- `callbackHelper.hpp`: unregister by a returned integer ID instead of comparing `std::function::target`, so destroying one statement no longer removes every statement's callback.

**ODBC entry-point exception barriers** (`src/driver`)
- Add a small helper, `src/driver/handles/entryGuard.hpp`. It takes a lambda, catches `std::exception` and `...`, records HY000 on the handle, and returns SQL_ERROR.
- Wrap these entry points with it: SQLFetch, SQLGetData, SQLTables, SQLColumns, SQLDescribeCol, SQLNumResultCols, SQLColAttribute, SQLFreeHandle, SQLCancel, SQLCancelHandle, SQLMoreResults, SQLGetInfo, SQLDisconnect, SQLAllocHandle, and the pre-try section of SQLDriverConnect.
- Add `catch (...)` to the existing try blocks.
- `SQLFetch`: check `hasError()` and return SQL_ERROR, not SQL_NO_DATA. Turn the recursion into a loop.
- `SQLGetData`: return HY010 (function sequence error) if called before the first fetch.
- `stringFromChar`: return `""` for a NULL pointer.
- Memory safety:
  - `getStmtAttr.cpp:89`: remove the memset and return HY092.
  - `getConnectAttr.cpp`: `SQL_ATTR_CONNECTION_DEAD` returns `SQL_CD_FALSE`/`SQL_CD_TRUE` as a `SQLUINTEGER`. `AUTOCOMMIT` and `LOGIN_TIMEOUT` become readable.
  - `setStmtAttr.cpp:28`: read integer attributes by value.
  - `valuePtrHelper.hpp`: string writers truncate to `BufferLength`, report the full length, and return `SQL_SUCCESS_WITH_INFO` with 01004.
- `getDiagRec.cpp`:
  - Return `statement->setError()` errors before trinoQuery errors.
  - Fix the chunking loop for `BufferLength` 0 or 1 or an over-long line, so it always reaches SQL_NO_DATA.

## Phase 3 — Finish prepared statements

- `prepare.cpp`:
  - Null-check the handle.
  - Clear `lastPreparedStatement` in `TrinoQuery::reset()`.
  - After polling, check `hasError()` and return SQL_ERROR with the Trino message.
  - Issue `DEALLOCATE PREPARE` for the previous name when a statement is re-prepared or freed, so the `X-Trino-Prepared-Statement` header doesn't grow without limit.
- `execute.cpp`, `descriptorFieldToParameterText`:
  - Honour `StrLen_or_IndPtr`: `SQL_NULL_DATA` → `NULL`, explicit lengths for `SQL_C_CHAR`, and `SQL_NTS`.
  - Add `SQL_C_TYPE_DATE/TIME/TIMESTAMP` (91/92/93), timestamp fractions, `SQL_C_NUMERIC` (via the existing `decimalHelper`), `SQL_C_BINARY` → `X'..'`, and `SQL_C_DEFAULT` (derived from the SQL type).
  - Reject NaN/Inf with a 22003 (numeric value out of range) error.
  - Wrap the literal in `CAST(… AS <type>)` when the `ParamType` (SQL type) differs from the C type's natural type.
  - Poll `UntilColumns` after the POST so errors surface at execute time.
- `SQLNumParams`: count `?` outside quoted strings, identifiers, and comments. Put that counting in a new pure helper, `src/util/paramCounter.{hpp,cpp}`, so it can be unit-tested.
- `SQLDescribeParam`: new `describeParam.cpp`. It returns `SQL_VARCHAR` / `SQL_NULLABLE_UNKNOWN`. That satisfies apps that probe parameter types and keeps the existing `SQL_DESCRIBE_PARAMETER="Y"` answer honest.
- `SQLParamData` / `SQLPutData`: implement data-at-exec for `SQL_C_CHAR`/`BINARY`.
  - `SQLExecute` returns `SQL_NEED_DATA` when any indicator is `SQL_DATA_AT_EXEC` or `SQL_LEN_DATA_AT_EXEC(n)`.
  - `SQLParamData` iterates over those parameters, and `SQLPutData` appends to a per-parameter buffer.
  - The final `SQLParamData` runs the execute.
- Separate the application parameter descriptor from the implementation one (`statementHandle.cpp:79`), so `SQLFreeStmt(SQL_CLOSE)` keeps parameter bindings. Implement `SQL_RESET_PARAMS` and `SQL_UNBIND` in `freeStmt.cpp`.
- Update the README "Known Limitations" section: parameterized queries are now supported.

## Phase 4 — HTTP resilience and diagnostics (ROADMAP §5 phases 1–2, AGENTS #14)

- New pure helper `src/trinoAPIWrapper/retryPolicy.{hpp,cpp}`:
  - Fibonacci backoff (1, 1, 2, 3, 5 … s, with ±20% jitter, capped at 30 s), up to a maximum number of attempts.
  - Parses `Retry-After` (seconds or HTTP-date), `RateLimit-Reset`, and `RateLimit-Remaining`.
  - Must not include `sql.h`/`sqlext.h` or other ODBC headers.
- In `post()` and `poll()`:
  - On 429/502/503/504, retry using the policy.
  - When `Retry-After` or `RateLimit-Reset` is present, sleep that long instead.
  - If `RateLimit-Remaining` is 0 or 1, raise the poll interval floor.
- Response header parsing reuses the existing `responseHeaderData` callback.
- `poll()`:
  - Check the HTTP status.
  - On curl failure, count it against the same retry budget, then fail instead of looping forever.
  - Cap the linear poll backoff at 1 s.
- Structured errors: on a non-2xx response, try to parse the body as Trino JSON (`error.message`, `errorName`, `errorCode`) and fall back to the first 512 bytes of the body. Store the result in `odbcError` via the existing `TrinoOdbcErrorHandler::FromTrinoJson`, so `SQLGetDiagRec` shows it.
  - Map the SQLSTATEs: 401/403 → 28000, 429/503 after retries → HYT01, `QUERY_QUEUE_FULL` → HYT00.
- Move the `sql.h` include out of `trinoOdbcErrorHandler.hpp` (layering rule) by keeping SQLSTATE values as plain strings in the wrapper layer.
- Make the mutable globals `static const` (`API_POLL_INTERVAL_MS`, `EXPIRY_GRACE_PERIOD_S`, …).
- `trinoOdbcErrorHandler.cpp`:
  - Set the initialized flag only after the catalog is filled.
  - Return entries by value instead of a pointer into the map.
  - Stop stat-ing the file on every error.

## Phase 5 — High-value connection features

Each one follows the AGENTS.md "Adding a new connection parameter" checklist: `DriverConfig` → defaults → `applyKVP` → `driverConfigToKVPs` (unconditional) → `profileReader.cpp` → `dsnConfigForm.cpp` → README tables.

- **Login timeout (#6):**
  - `Connection::configure()` copies `ATTR_LoginTimeout` into a new `ConnectionOptions::connectTimeoutMs`.
  - Set `CURLOPT_CONNECTTIMEOUT_MS` from it (default 10 s when it is 0).
  - The first `/v1/info` call is bounded by it as well.
- **SSLCAInfo directory (#7):** if `std::filesystem::is_directory(path)`, set `CURLOPT_CAPATH`, otherwise `CURLOPT_CAINFO`.
- **Proxy (#12):** new `Proxy` and `ProxyUsername`/`ProxyPassword` parameters.
  - `ProxyPassword` goes in `SECRET_KEYS` and is DPAPI-encrypted.
  - Set `CURLOPT_PROXY`/`CURLOPT_PROXYUSERPWD`.
  - Empty value → curl's environment-variable behaviour.
- **DSN form validation (#8):** on Save, validate the port (1–65535), check that the timeout is a positive integer and that SSLVerify is a recognised word, and show a `MessageBox` instead of saving.
  - Put the validators in a pure helper, `src/driver/config/dsnValidation.{hpp,cpp}`, so they can be unit-tested.
- **SQLGetInfo (#9):**
  - Add `SQL_DATA_SOURCE_NAME`, `SQL_DATABASE_NAME` (catalog), `SQL_TXN_CAPABLE` = `SQL_TC_NONE`, `SQL_DATA_SOURCE_READ_ONLY` = "Y", `SQL_IDENTIFIER_CASE`, `SQL_OJ_CAPABILITIES`, `SQL_TABLE_TERM`, `SQL_KEYWORDS`, `SQL_MAX_*_NAME_LEN`, `SQL_SCROLL_OPTIONS` = `SQL_SO_FORWARD_ONLY`, `SQL_CURSOR_SENSITIVITY`, and `SQL_*_CURSOR_ATTRIBUTES1/2`.
  - Unknown types: log at WARN and return `SQL_SUCCESS` with an empty string or 0, as AGENTS.md suggests.
  - Handle a null `InfoValue` correctly (still report the length).
- **SQLGetFunctions accuracy:**
  - Report only implemented functions. Drop the stubs: FetchScroll, ExtendedFetch, Statistics, SetDescField/Rec, CopyDesc, cursor names, NativeSql, DataSources/Drivers, and SetScrollOptions.
  - Add NumParams, ParamData, PutData, and DescribeParam once Phase 3 implements them.
- **SQLTables/SQLColumns injection fix:** escape `'` in catalog, schema, table, and column filters. Strip existing quotes from table-type list items before re-quoting them.

## Phase 6 — Tests and CI

- New **pure unit tests** under `test/unit/` that need no DSN and no network, built as a separate `UnitTests` target in `CMakeLists.txt` so they run in CI:
  - `tokenParserTest`: JWT with sub/preferred_username/email, opaque token, malformed token.
  - `userHeaderTest`: claim precedence for X-Trino-User (AGENTS #4).
  - `tokenExpiryTest`: grace-period logic, missing `exp`.
  - `authLoopGuardTest`: a fake `AuthConfig` whose refresh always fails. Drive the `isExpired`/`refresh` loop logic and assert that it terminates (AGENTS #2). This requires extracting the loop into a small testable function `ensureFreshAuth(AuthConfig*)`, which `getCurl()` calls.
  - `retryPolicyTest`, `paramCounterTest`, `dsnValidationTest`, and NULL/indicator/TYPE_* cases added to the existing `testExecute.cpp`.
- Fix the existing `testExecute.cpp` including `execute.cpp`, which causes a duplicate `SQLExecute`: move `descriptorFieldToParameterText` into `src/driver/paramLiteral.{hpp,cpp}`.
- New workflow `.github/workflows/build.yml`:
  - On push/PR, runs on windows-2022: vcpkg, then `cmake --preset x64-release`, then build, then run `UnitTests`.
  - Gives the real MSVC verification that is impossible here. It also starts a Trino Docker service where possible, but integration tests stay manual.
- The Kerberos handle-restore test (AGENTS #3) needs a domain. Document it as a manual checklist item; the DELETE/POSTFIELDS reset in `getCurl()` covers the regression structurally.

## Phase 6b — M365 Power BI on-premises gateway: OIDC (Entra ID) and LDAP

**Target.** The gateway runs Power Query in `Microsoft.Mashup.Container` processes under the `NT SERVICE\PBIEgwService` account, in session 0, 64-bit only, with several containers running at once. It needs a **System DSN**.

There are two supported paths. Both are non-interactive and already exist in the code, but both have defects that block them today.

### LDAP path

Trino uses its LDAP password authenticator over HTTPS. The driver uses `AuthMethod=Basic Auth`.

- In the Power BI service, the gateway data source uses the **Basic** credential type. Power Query then appends `UID=…;PWD=…` to the connection string.
- Precedence already works: `pwd` and `uid` are applied after the DSN's `password`/`username` (`driverConfig.cpp:505-509`). **Fix:** add a unit test for precedence in `driverConfigFromKVPs`. The case to cover: the DSN has a username and password, the connection string has UID/PWD, and the connection string wins. Also cover an empty PWD, which must not clear the DSN value.
- **Fix (security):** `driverConnect.cpp` logs the raw connection string and the merged KVPs at TRACE. That leaks the LDAP password and the decrypted DSN secrets (`clientSecret`, `password`, `token`) into the log file.
  - Add `redactSecrets(kvps)` / `redactConnStr()` in `src/util/`, covering `pwd`, `password`, `clientsecret`, `token`, `proxypassword`, and `encrypted*`.
  - Use them at every place that logs KVPs or connection strings.
  - Unit-test them.
- On a 401 from Trino, return SQLSTATE `28000` with Trino's message. The gateway's "Test connection" dialog shows only the driver's diagnostic text, so this message is what the administrator sees. This is covered by the Phase 4 error mapping.

### OIDC path

The gateway uses the OAuth 2.0 client-credentials flow against **Microsoft Entra ID** (`AuthMethod=OIDC Client Cred Auth`).

- In the Power BI service, the gateway data source uses the **Anonymous** credential type. The client secret lives only in the System DSN, DPAPI-encrypted with machine scope.
- **Fix (blocking for Entra):**
  - `scope` is built but never sent (`clientCredAuthProvider.cpp:71-75`). Entra rejects a client-credentials request that has no `scope=api://<app-id>/.default`.
  - Extract `buildClientCredRequestBody(...)` as a pure function and unit-test it: grant type, client_id, secret, and scope, all URL-encoded.
- **Fix:**
  - Don't send Trino's request headers, including any stale Bearer token, to `login.microsoftonline.com`. Use a clean header list for the discovery and token calls.
  - Check the HTTP status and surface Entra's `error_description` (e.g. `AADSTS7000215 invalid client secret`) as the diagnostic.
- **Fix:**
  - Entra access tokens live 60–90 minutes. The new grace-period rule (min of 600 s and 10% of the lifetime) prevents the refresh loop.
  - Entra client-credential tokens have **no `preferred_username` or `email` claim**, so `X-Trino-User` falls back to `sub`, which is the service principal's object ID. That matches Trino's default `http-server.authentication.oauth2.principal-field=sub`.
  - Add one new DSN option, `UserClaim` (default empty, meaning preferred_username → sub → email). It picks which claim becomes `X-Trino-User`, for example `oid`, `azp`, or `appid`, so administrators can match whatever `principal-field` Trino is configured with.
  - Add it via the "new connection parameter" checklist, and unit-test it alongside `userHeaderTest`.
- **Fix (concurrency):**
  - Several Mashup containers share `%LOCALAPPDATA%\Temp\TrinoODBCTokenCache.json` under the service profile.
  - Wrap reads and writes in a named mutex (`Local\TrinoODBCTokenCache`). Write to a temporary file and then replace the cache with `MoveFileExA(MOVEFILE_REPLACE_EXISTING)`. A corrupt cache counts as empty.
  - Client-credential tokens are cheap to re-fetch, so a cache failure must never fail the connection.
- `isInteractiveDesktopAvailable()` already rejects External Auth and Device Flow in session 0. Keep it. The error message should point to Client Cred, Basic, Token, and Kerberos.

### ODBC API surface Power Query uses

The goal is to tolerate everything it calls rather than return a hard SQL_ERROR.

- `SQLSetStmtAttr`:
  - Accept `SQL_ATTR_QUERY_TIMEOUT` (store it and apply it as a deadline in `poll()`, then send a DELETE through cancel), `SQL_ATTR_MAX_ROWS`, `SQL_ATTR_ROW_ARRAY_SIZE=1`, `SQL_ATTR_CURSOR_TYPE=FORWARD_ONLY`, `SQL_ATTR_CONCURRENCY=READ_ONLY`, `SQL_ATTR_NOSCAN`, and `SQL_ATTR_RETRIEVE_DATA`.
  - Unsupported values → `SQL_SUCCESS_WITH_INFO` with SQLSTATE `01S02` (option value changed), as the ODBC specification requires.
  - Unknown attributes → `HY092` (invalid attribute) with a diagnostic.
  - `SQLGetStmtAttr` mirrors these.
- `SQLSetConnectAttr`:
  - Accept `SQL_ATTR_CONNECTION_TIMEOUT`, `SQL_ATTR_ACCESS_MODE`, `SQL_ATTR_TXN_ISOLATION`, `SQL_ATTR_CURRENT_CATALOG` (sets `X-Trino-Catalog`), `SQL_ATTR_ANSI_APP`, and `SQL_ATTR_RESET_CONNECTION` (pooling).
  - Fix `SQL_ATTR_CONNECTION_DEAD`. It is already in Phase 2, but it is critical here because the gateway pools connections.
- `SQLGetTypeInfo`: return an empty result set for types it doesn't cover, instead of SQL_ERROR.
- `SQLGetInfo` unknown-type fallback (Phase 5). Power Query's query-folding probes many InfoTypes.
- `SQLDriverConnect`:
  - Honour `SQL_DRIVER_NOPROMPT`, and never show the dialog in session 0.
  - Output-string truncation must return `01004` with the full length. Today it silently writes nothing.

### Deliverable: `docs/power-bi-gateway.md` runbook

- Driver install (x64 MSI) and System DSN creation.
- Trino server configuration for each path:
  - LDAP: `password-authenticator.properties`, HTTPS.
  - Entra: `http-server.authentication.type=oauth2,jwt`, JWKS URL, `principal-field`, `aud`. Also the Entra app registration with "Expose an API" and the `.default` scope.
- Example DSNs for each path, plus the Power BI service data source setup (Basic for LDAP, Anonymous for OIDC).
- Proxy and private-CA setup.
- How to capture a driver TRACE log and an ODBC driver-manager trace when something fails. Any new missing call should be logged as a feature gap, following ROADMAP §6.

## Phase S — Security hardening

A security review of the code and of this plan found issues that the earlier phases did not cover. They are ranked by severity, and every one is fixed in this phase.

**Threat model.** Assets:
- the LDAP password
- the Entra client secret
- access tokens
- query results

Attackers:
- (a) another local user on the gateway host
- (b) whoever controls the connection string, such as a report author or an app that builds connection strings
- (c) the network, or a malicious or compromised endpoint that the driver talks to

### Critical

- **S1 — Logging defaults to TRACE, to a world-writable path.**
  - `writeLog.cpp:15` sets `DEFAULT_LOG_LEVEL = LL_TRACE`, and the log goes to the hard-coded `C:\temp\odbclog.txt`. Everything before the DSN's `LogLevel` is applied gets logged, including the raw connection string with `PWD=`.
  - Any local user can read `C:\temp`, or pre-create the file or plant a link there.
  - Fix:
    - Default level `LL_OFF` (or `LL_WARN`).
    - Move the log path to `%LOCALAPPDATA%\TrinoODBC\logs\` (for the gateway, that is the service profile), created with an owner-only DACL. Add an optional `LogPath` DSN key.
    - Cap the file size and rotate it.
    - Apply `redactSecrets` everywhere (Phase 6b). Never log response bodies, which contain query rows, below TRACE. Mark TRACE as "may contain data" in the docs.
- **S2 — Credentials are sent over cleartext HTTP.**
  - `Hostname` may be `http://…`. Basic and Bearer credentials go out before Trino rejects them. The statement in `notes.md` that "it fails rather than leaking" is wrong, because the leak has already happened.
  - Fix: `ConnectionConfig` refuses every credential-bearing method (Basic, Token, ClientCred, External, DeviceFlow, Kerberos) unless the scheme is `https://`. The only exception is `localhost` or `127.0.0.1`, which tests need.
  - Error message: "AuthMethod X requires https://".
- **S3 — Connection-string override leads to credential theft or code execution.**
  - `SQLDriverConnect` lets the connection string override any DSN value.
  - Someone who controls the connection string but not the DSN can use `dsn=Prod;hostname=https://evil` or `tokenEndpoint=https://evil`. The driver then sends the DSN's stored password or client secret to them.
  - With `tokenCommand=…`, they run arbitrary commands as `PBIEgwService`.
  - Fix: when a DSN is named, a fixed `DSN_LOCKED_KEYS` set can **only** come from the DSN, and connection-string values for those keys are ignored with a WARN.
    - The set: `hostname`, `port`, `authmethod`, `oidcDiscoveryUrl`, `tokenEndpoint`, `clientId`, `tokenCommand`, `tokenFile`, `sslVerify`, `sslCAInfo`, `proxy*`, `servicePrincipalName`, `logPath`.
  - `UID`/`PWD` stay overridable, because that is how Power BI passes LDAP credentials.
  - A DSN-less connection string is unaffected: nothing is stored, so nothing can be stolen.
  - Unit-test both cases.

### High

- **S4 — Server-supplied URLs are followed blindly.**
  - `nextUri` and `partialCancelUri` from Trino responses get the `Authorization` header attached.
  - Fix: allow them only when they keep the configured scheme, host, and port. A downgrade from https to http is always refused, with a message pointing to `http-server.process-forwarded=true`, the usual cause behind a TLS-terminating load balancer.
  - For Trino Gateway or load-balancer setups where the host changes, add a DSN-only `AllowedHosts` list.
  - Also:
    - Set `CURLOPT_PROTOCOLS_STR "https,http"`.
    - Set `CURLOPT_REDIR_PROTOCOLS_STR "https"`, with redirects left off as they are today.
    - Set `CURLOPT_SSLVERSION` to TLS 1.2 or later.
- **S5 — Server-supplied URLs are opened with `ShellExecute` or `system()`.** This covers `externalAuthProvider.cpp:121` and deviceFlow.
  - Fix: allow only the `https://` scheme. Reject `file:`, custom protocol handlers, and anything else before calling `openURLInDefaultBrowser`, and drop `std::system`. This matters on the desktop; on the gateway, session-0 detection already blocks these flows.
- **S6 — Secrets in a System DSN can be decrypted by any local user.**
  - `CRYPTPROTECT_LOCAL_MACHINE` plus `HKLM\SOFTWARE\ODBC\ODBC.INI`, which `Users` can read, means any interactive user on the gateway host can recover the Entra client secret.
  - Fix, in two parts:
    1. **Entra certificate credential (recommended by Microsoft over secrets):**
       - Add `ClientCertThumbprint`. The driver builds a `private_key_jwt` `client_assertion` (RS256, signed with CNG `NCryptSignHash` using a non-exportable key in `LocalMachine\My`, private key ACL'd to `PBIEgwService`).
       - No secret is stored in the DSN at all.
       - New pure helper `clientAssertion.{hpp,cpp}`. The JWT assembly and base64url encoding are unit-tested; the signing is behind an interface.
    2. When a secret is still used, `ConfigDSN` tightens the DSN's registry key ACL to SYSTEM, Administrators, and an optional `ServiceAccount` SID. The runbook covers this too.
  - LDAP path: recommend letting Power BI supply UID/PWD at runtime ("Basic" credential type) rather than storing the password in the DSN.

### Medium

- **S7 — Error-map JSON is loaded from untrusted locations.**
  - `trinoOdbcErrorHandler.cpp:60-76` falls back to the **current working directory** and to an environment variable, and it re-stats the file on every error.
  - Fix: load only from the driver's module directory, once at startup.
- **S8 — Token cache location.**
  - The cache sits in `%LOCALAPPDATA%\Temp`. Its entries are DPAPI user-scope encrypted, which is good.
  - Fix: move it to `%LOCALAPPDATA%\TrinoODBC\` with an owner-only DACL, and add the locking and atomic write from Phase 6b. Client-credential tokens skip the disk cache entirely, because they are cheap to re-fetch and it keeps one less artifact on disk.
- **S9 — Secret lifetime in memory.**
  - Zero the password, secret, and token strings when the `ConnectionConfig` is destroyed (`SecureZeroMemory`).
  - Don't keep the Basic `user:pass` string after the header has been built.
  - Reply to Kerberos/NTLM proxy 407 challenges using only the configured proxy credentials.
- **S10 — Response-size denial of service.** Cap response bodies (default 256 MiB per request) and the number of headers, so a hostile endpoint can't exhaust the host's memory.
- **S11 — `SSLVerify=false`.** It is a DSN-locked key (S3). It is never allowed together with Basic auth to a non-localhost host unless `SSLCAInfo` is also set; otherwise the driver gives an actionable error. The WARN log is kept.

### Process

- Pin and refresh the vcpkg baseline, and record the curl and OpenSSL versions in the release notes.
- Keep the CodeQL workflow (`pull-request-code-scan.yml`) running on the new build workflow.
- Authenticode-sign the DLL and MSI (documented in `HOW_TO_RELEASE.md`; it needs the user's certificate).
- Treat JWT claims as **untrusted display hints only**. The driver never makes authorization decisions from them.

### Residual risks, stated in the doc

- `TokenCommand` still runs whatever an administrator configures.
- Any administrator on the gateway host can recover any credential.
- TRACE logs can contain query data.
- This review is static code reading, not a penetration test. An independent review is recommended before enterprise rollout.

## Phase D — Repo documents (first execution step)

1. `docs/production-readiness.md`: this plan, the audit findings (with file:line), and a status checklist per phase, updated as work lands.
2. `docs/security.md`: the threat model, findings S1–S11 and the Phase 2 crash and DoS items, the mitigations, configuration guidance (registry ACL, certificate credential, log location), and the residual risks. Link it from `README.md`, and fix the incorrect "Security posture" claim in `notes.md`.
3. `docs/power-bi-gateway.md`: the runbook from Phase 6b, including the secure-by-default DSN examples.

## Phase P — ODBC conformance for Power BI / Power Query

Power BI's generic ODBC connector (`Odbc.DataSource`, used by Desktop and by the gateway) needs these ODBC behaviours. Earlier phases do **not** cover them, and they decide whether data comes back *correct*, not just whether the connection works.

- **P1 — Unicode (blocking for non-ASCII data).**
  - The driver is ANSI-only and exports no `*W` functions. Power Query calls the Unicode API, so the Driver Manager converts between UTF-16 and the machine's *ANSI code page*.
  - Trino sends UTF-8. As a result, any non-ASCII text (`Müller`, `東京`, `€`) in data, column names, or table names is corrupted on a standard Windows install.
  - Fix:
    - Make it a Unicode driver. Export the `*W` variant of every function that takes strings (SQLDriverConnectW, SQLConnectW, SQLExecDirectW, SQLPrepareW, SQLTablesW, SQLColumnsW, SQLDescribeColW, SQLColAttributeW, SQLGetInfoW, SQLGetDiagRecW, SQLGetConnectAttrW, SQLSetConnectAttrW, SQLGetStmtAttrW, SQLSetStmtAttrW, SQLNativeSqlW, SQLGetTypeInfoW, SQLPrimaryKeysW, SQLForeignKeysW, SQLStatisticsW, SQLSpecialColumnsW).
    - Each one is a thin UTF-16↔UTF-8 wrapper over the existing ANSI implementation, using a pure, unit-tested `src/util/utf.{hpp,cpp}`. Note that the Driver Manager treats a driver as Unicode only if it exports the W functions, so this has to be all of them or none.
    - Add `SQL_C_WCHAR` to `rowToBuffer.cpp` and `bindCol`.
    - Report string columns as `SQL_WVARCHAR` (SQLDescribeCol/ColAttribute/Columns/GetTypeInfo).
    - Lengths are reported in bytes, as the spec requires.
  - This reverses the "out of scope" note in AGENTS.md. Update that file.
- **P2 — Chunked `SQLGetData` (blocking for long text).**
  - After a 01004 truncation, the next `SQLGetData` call on the same column restarts from offset 0 instead of continuing, and it never returns `SQL_NO_DATA`.
  - Power Query reads long VARCHAR or JSON values in pieces, so values longer than its buffer come back duplicated or the read loops.
  - Fix:
    - Track `(column, byteOffset)` per statement. Continue from the offset, return `SQL_NO_DATA` once the value is exhausted, and reset the offset on fetch or on a different column.
    - For WCHAR, never split a UTF-16 surrogate pair.
    - Report the remaining length in `StrLen_or_IndPtr`.
- **P3 — Catalog functions Power Query may probe.**
  - Export `SQLPrimaryKeys`, `SQLForeignKeys`, `SQLSpecialColumns`, and `SQLStatistics`. Each returns a correctly shaped **empty** result set, since Trino has no keys or indexes.
  - Power Query's `CreateNavigationProperties` (default on) and some query-folding paths call them, and an IM001 or SQL_ERROR there can abort navigation.
  - Report them in `SQLGetFunctions`.
- **P4 — Accurate capability reporting.**
  - Phase 5 (GetFunctions/GetInfo honesty) is required here: Power Query picks its code paths from these answers.
  - Also return correct values for `SQL_SQL_CONFORMANCE`, `SQL_ODBC_INTERFACE_CONFORMANCE` (Core, not overclaimed), `SQL_GETDATA_EXTENSIONS` (`SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER` only if true after P2), `SQL_DATA_SOURCE_READ_ONLY`, `SQL_TXN_CAPABLE`, and the `SQL_*_CURSOR_ATTRIBUTES*` entries for forward-only, read-only cursors.
- **P5 — Column metadata.**
  - `SQLDescribeCol` and `SQLColAttribute` must give a bounded `COLUMN_SIZE` for unbounded `varchar`. Power Query allocates buffers from it; use 65535 and set `SQL_DESC_LENGTH` accordingly. P2 makes longer values still readable.
  - Report `SQL_NULLABLE_UNKNOWN` rather than claiming every column is nullable.
  - Map decimal precision and scale, and `timestamp(p)` / `timestamp with time zone`, to the correct SQL types.
- **P6 — Statement and connection attributes.** Phase 6b tolerance list (01S02 and HY092 instead of a hard SQL_ERROR), query timeout, and `SQL_ATTR_CONNECTION_DEAD` for pooling.
- **P7 — Verification with Power BI itself.**
  - Turn on ODBC Driver Manager tracing and the driver's TRACE log. Then run Power BI Desktop through: navigator (catalog, schema, table), loading a table that has non-ASCII and more-than-8-KB string values, a filtered and folded query, and a refresh through the gateway.
  - Any call that returns an error in the trace is a feature gap (ROADMAP §6).
  - Capture a reference trace in `docs/power-bi-gateway.md`.

**Execution order:** add P2, P5, and P6 right after Phase 2. P1 (Unicode) and P3 go before the gateway rollout.

## Phase 7 — Docs

- Update `CHANGELOG.md` (Unreleased), `notes.md` (status, new gaps, verification checklist additions), `AGENTS.md` (mark completed items, document `entryGuard`, `retryPolicy`, `ensureFreshAuth`), and the `README.md` parameter tables and limitations.
- State plainly that this was syntax-checked with MinGW, not built with MSVC, until CI passes.

---

## Critical files

- `src/trinoAPIWrapper/connectionConfig.{hpp,cpp}`, `trinoQuery.{hpp,cpp}`, `trinoOdbcErrorHandler.{hpp,cpp}`, `connectionOptions.hpp`
- `src/trinoAPIWrapper/authProvider/{tokenCacheAuthProviderBase,clientCred,external,deviceFlow,token,kerberos}AuthProvider.cpp`, `tokens/{tokenCache,tokenParser}.cpp`
- `src/driver/{fetch,getData,tables,columns,prepare,execute,bindParameter,numParams,paramData,putData,freeStmt,getInfo,getFunctions,getDiagRec,getStmtAttr,getConnectAttr,setStmtAttr,driverConnect}.cpp`, `handles/statementHandle.cpp`, `handles/connHandle.cpp`
- `src/driver/config/{driverConfig,profileReader,configDSN,dsnConfigForm}.cpp`
- `src/util/{valuePtrHelper.hpp,stringFromChar.cpp,callbackHelper.hpp,windowsLean.hpp}`
- `CMakeLists.txt`, the new `.github/workflows/build.yml`

## Verification

1. A MinGW `-fsyntax-only` pass over all of `src/**/*.cpp` is clean.
2. The new pure unit tests build and pass natively with Linux g++ + gtest. Logic tests for the token parser, expiry, loop guard, retry policy, parameter counter, DSN validation, and parameter literals are all host-independent.
3. `grep` confirms that every `SQLRETURN SQL_API` function body is wrapped by `entryGuard` or by a try with `catch (...)`.
4. Gateway-specific unit tests pass natively:
   - UID/PWD precedence
   - secret redaction
   - the client-credentials request body includes `scope`
   - the `UserClaim` choice with an Entra-shaped token (no `preferred_username`; `sub`, `oid`, `azp` present)
   - grace-period handling of a 60-minute token
   - the stmt/connect attribute tolerance table
5. Security unit tests:
   - Locked keys are ignored when a DSN is named.
   - Credential methods over http:// are refused, except localhost.
   - nextUri with a different host or an https→http downgrade is refused.
   - Browser URL scheme validation.
   - Redaction.
   - client_assertion JWT shape (header `x5t#S256`, `aud` = token endpoint, `exp` ≤ 10 min, unique `jti`).
   - No secret strings appear in a TRACE log produced by a full connect path with a mocked transport.
6. **What only you can verify** (it needs your tenant and gateway). Follow `docs/power-bi-gateway.md`:
   - (a) LDAP: gateway "Test connection" with Basic credentials, then a scheduled refresh.
   - (b) Entra client credentials: Test connection with Anonymous credentials, then a refresh that runs longer than the token lifetime.
   - (c) A wrong password or secret produces a readable diagnostic, not a hang.
   - (d) Two datasets refreshing at the same time (concurrent containers) both succeed.
   - (e) The TRACE log contains no secrets.
7. Hand to the user:
   - Push to GitHub so `build.yml` performs the real MSVC build plus unit tests.
   - Then run the `notes.md` manual checklist (No Auth against a Trino container, the full TestDriver suite, an expired TokenCommand giving a 401 rather than a hang, a Kerberos query after the handshake, and Power BI gateway smoke tests).
