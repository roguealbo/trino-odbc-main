# Changelog

All notable changes to this driver are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

Changes in this release extend the open-source driver so it can be used
unattended from the Microsoft Power BI **on-premises data gateway**, which runs
as a Windows service and therefore cannot complete any interactive
authentication flow. The parameter model is deliberately close to the
ClickHouse OSS ODBC driver, since that is the reference most Power BI ODBC
setups are copied from.

### Added

#### Authentication

- **Basic Auth** (`AuthMethod=Basic Auth`). Sends
  `Authorization: Basic base64(user:password)` plus `X-Trino-User`. New
  `basicAuthProvider.{hpp,cpp}`. Trino accepts Basic credentials over HTTPS
  only.
- **Token Auth** (`AuthMethod=Token Auth`). Sends `Authorization: Bearer <JWT>`
  plus `X-Trino-User`. New `tokenAuthProvider.{hpp,cpp}`. The token may come
  from one of three sources, in order of precedence:
  - `Token` — a JWT stored in the DSN (DPAPI-encrypted),
  - `TokenFile` — a path to a file, re-read when the token expires,
  - `TokenCommand` — a command line that prints the JWT to stdout, re-run when
    the token expires. This is the hook for vault CLIs and sidecars.
- **Kerberos Auth** (`AuthMethod=Kerberos Auth`). Performs an HTTP Negotiate
  handshake through Windows SSPI (`AcquireCredentialsHandle` /
  `InitializeSecurityContext`), so no secret is stored in the DSN. New
  `kerberosAuthProvider.{hpp,cpp}`. `ServicePrincipalName` defaults to
  `HTTP/<hostname>`.
- `AM_BASIC_AUTH`, `AM_TOKEN_AUTH`, `AM_KERBEROS_AUTH` in `ApiAuthMethod`, plus
  `isNonInteractiveAuthMethod()` used to decide whether a method can run
  headless.
- `AuthCredentials` (`authProvider/authCredentials.hpp`) — credential material
  shared by the providers, kept in the API-wrapper layer so the wrapper does not
  depend on driver-layer DSN parsing.
- `ConnectionOptions` (`connectionOptions.hpp`) — transport and
  client-identification settings.

#### Connection parameters

- `Username` — the identity sent as `X-Trino-User`, now configurable for every
  method instead of hard-coded.
- `Password`, `Token`, `TokenFile`, `TokenCommand`, `ServicePrincipalName`.
- `SSLCAInfo` — path to a CA bundle or directory for a private CA. Empty keeps
  the previous behaviour of using the Windows certificate store.
- `SSLVerify` — disables peer-certificate verification for self-signed
  coordinators. Accepts `false`/`off`/`no`/`0`. Logs a `WARN` when off, because
  it is a security downgrade.
- `RequestTimeoutMs` — replaces the fixed 10-second per-request timeout, which
  was too short for the long result-set polls that Power BI imports generate.
- `ClientInfo` — identifies the calling application in the `User-Agent`, so
  several reports sharing one gateway can be told apart in Trino's query log.
- `UID` and `PWD` are accepted as aliases for `Username` and `Password` in
  connection strings, matching what Microsoft clients and the ClickHouse driver
  send.

#### Credentials from the application, not the DSN

- `SQLConnect` now honours its `UserName` and `Authentication` arguments instead
  of ignoring them. An empty argument means "not supplied" and leaves the DSN
  value untouched.
- `SQLSetConnectAttr` accepts `SQL_ATTR_UID` (60) and `SQL_ATTR_PASSWORD` (61).
  Both are guarded with `#ifndef` because they are ODBC 3.8 attributes and are
  missing from older Windows SDK headers.
- Precedence is single-path: `SQLConnect` arguments and connect attributes
  override DSN values, applied in `Connection::configure()`.

#### Secrets at rest

- `Password` and `Token` are DPAPI-encrypted in `ODBC.INI` under
  `encryptedPassword` / `encryptedToken`, exactly as `ClientSecret` always has
  been. `configDSN.cpp` now drives this from a generic `SECRET_KEYS` list rather
  than a hard-coded single key, and `profileReader.cpp` decrypts all three.

#### Host-environment awareness

- `isInteractiveDesktopAvailable()` (`src/util/hostEnvironment.cpp`) detects a
  session-0 service host via `ProcessIdToSessionId`.
- `Connection::checkInputs()` rejects `External Auth` and `Device Flow` in a
  non-interactive host with an actionable message naming the methods that do
  work, instead of hanging until the request times out.

#### Diagnostics

- `SQLGetInfo(SQL_USER_NAME)` is implemented, reporting the identity the
  connection actually carries — which for token-based methods may come from a
  JWT claim rather than from the DSN.
- `SQLGetConnectAttr(SQL_ATTR_UID)` returns the same effective identity, so a
  client that supplied its own credentials can still confirm who the connection
  is acting as. `SQL_ATTR_PASSWORD` is deliberately *not* readable back.
- `Connection::disconnect()` and `getServerVersion()` tolerate a null
  `ConnectionConfig`, and `SQLGetInfo(SQL_SERVER_NAME)` returns empty in the
  same situation. A host application calls `SQLDisconnect` even after a failed
  connect, which previously dereferenced null and crashed the host during
  cleanup.
- `ConnectionConfig::getTrinoServerVersion()` no longer throws when `/v1/info`
  returns something unparseable (a proxy error page, for example). It logs a
  warning and returns an empty version, so `SQL_DBMS_VER` degrades instead of
  failing the connection.

### Fixed

- **The OIDC providers never sent `X-Trino-User`.** `tokenCacheAuthProviderBase`
  set only `Authorization`. Trino requires `X-Trino-User` on every request and
  rejects a mismatch with the token principal, so this was a latent failure for
  any deployment that enforces it. The header is now derived from the access
  token's `preferred_username`, `sub`, or `email` claim in a new
  `applyUserHeader()`, which is called from both `applyToken()` and
  `setAccessTokenHeader()`.
- **Stale bearer token after refresh.** `setAccessTokenHeader()` used
  `map::insert()`, which silently does nothing when the key already exists, so a
  refreshed token was never applied to an existing connection. It now assigns
  through `operator[]`.
- **Potential infinite loop on an unrenewable JWT.** `ConnectionConfig::getCurl()`
  loops `while (isExpired()) refresh()`. The token provider's post-refresh check
  called the guard-aware `isExpired()`, which always reports "not expired" once
  the guard is set — so the guard was cleared on every attempt and a permanently
  expired token spun forever. A guard-free `tokenIsExpired()` is now used for
  that check.
- **Kerberos left the shared CURL handle in POST mode.** The handshake borrowed
  the handle to POST a trigger query and never cleared `CURLOPT_POSTFIELDS`, so
  the next statement poll would have been a POST carrying the trigger query as
  its body. The handle is now restored to GET.
- **Kerberos could throw across the C ABI.** `refresh()` rethrew, and it is
  called from underneath ordinary ODBC entry points; an escaping exception would
  terminate the host process. Failures are now recorded and the request proceeds
  unauthenticated, so Trino's 401 becomes a normal SQL error.
- **Exceptions escaping `SQLConnect`.** Configuration failures now return
  `SQL_ERROR` with a `HY000` diagnostic through `SQLGetDiagRec` instead of
  unwinding out of the C ABI.
- **Empty connection-string values threw.** `port=;` or `requestTimeoutMs=;`
  reached `std::stoi`/`std::stol` and threw. The KVP helper now skips empty
  values, leaving the default in place.
- **`No Auth` hard-coded `TestUser`.** The username is now configurable; the
  `TestUser` value is retained only as the default, which keeps the bundled test
  suite passing against a vanilla Trino instance.
- **`SQLConnect` never marked the connection connected.** It now sets
  `connected` on success, matching `SQLDriverConnect`.

### Hardened (exception safety)

The driver is a DLL hosted by someone else's process, so an exception escaping
any entry point terminates Excel or Power BI. Several paths introduced or
exposed by this work could throw, and all of them now report instead:

- `SQLConnect` reads the DSN profile *inside* its try block. It can throw from
  secret decryption and from auth-method validation, and previously did so
  outside any handler.
- `SQLDriverConnect` likewise now covers profile reading and config
  construction, not just `configure()`.
- `ConfigDSN` catches around both the profile read (CONFIG case) and the
  encrypt-and-write path, returning a declined configuration rather than
  unwinding through the driver manager.
- The `DSNForm` constructor tolerates an unrecognised `authmethod` attribute
  instead of throwing; strict validation belongs at connect time, where it can
  become a SQL error.
- `setPort` and `setRequestTimeoutMs` no longer throw on non-numeric input,
  which previously unwound through the dialog's window procedure. They log and
  keep the default. `setPort` also rejects out-of-range values rather than
  letting them wrap into a `uint16_t`, and a non-positive timeout is ignored.
- `setLogLevel` falls back to the current value for an unknown name instead of
  raising `std::out_of_range` from `map::at()`.
- `setAuthMethod` reports an unknown method as an `invalid_argument` listing the
  valid names. It deliberately does **not** fall back to a default: silently
  keeping `No Auth` after a typo would connect unauthenticated.
- **Clearing a secret in the DSN dialog now actually clears it.**
  `driverConfigToKVPs` previously skipped empty values, so deleting a password
  left the old `encryptedPassword` in `ODBC.INI` and still in use. Every setting
  is now written unconditionally; `dsn` and `driver` are still emitted only when
  set, since they identify the entry rather than configure it. This stays safe
  for `SQLDriverConnect`, which uses the same function to fill DSN defaults,
  because `driverConfigFromKVPs` ignores empty values.

### Changed

- `ConnectionConfig`'s constructor takes a `ConnectionSetup` struct instead of
  twelve positional strings, removing the class of bug where two same-typed
  arguments get transposed.
- The DSN configuration form is a two-column layout with all fields visible at
  once. Fields that the selected auth method does not use are greyed out and
  filled with a `<Not Required>` placeholder, which is read back as an empty
  string so it is never written to the DSN.
- The Win32 control makers accept position and width arguments
  (`editMaker`, `labelMaker`, `comboboxMaker`, `buttonMaker`), and a new
  `passwordMaker` masks the password, client secret, and JWT fields.
- `ClientSecret`, `Password`, and `Token` are masked in the form.
- The "does not support most forms of Trino authentication including password
  authentication" limitation has been removed from the README.

### Build

- New sources added to `CMakeLists.txt` for the three providers and
  `hostEnvironment.cpp`.
- `secur32` is linked for the SSPI handshake.

---

### Not yet verified

This work has **not been compiled**. The driver is Windows/MSVC-only and the
environment it was written in has no compiler, so the changes need a build with
MSVC + vcpkg (or the repository's GitHub Actions workflow) before they are
trusted. See `notes.md` for the specific things to check first.

[Unreleased]: https://github.com/trinodb/trino-odbc/compare/main...HEAD
