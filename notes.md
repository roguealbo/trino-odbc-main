# Implementation Notes

Working notes for the non-interactive authentication and Power BI gateway work.
This is not user documentation — see `README.md` for that, `CHANGELOG.md` for
what changed, and `AGENTS.md` for how to continue.

## Status: written, not compiled

There is no compiler in the environment this was written in (no `g++`,
`clang++`, `cmake`, or mingw), and the driver is Windows/MSVC-only. **Nothing
here has been built.** Treat every file as needing a first compile pass.

Build it with the repository's own path: Visual Studio + vcpkg, or the
`make_release.yml` workflow. `vcpkg.json` already carries `curl[openssl]`,
`nlohmann-json`, and `gtest`, and no new third-party dependency was introduced —
Kerberos uses SSPI and base64 uses `crypt32`, both already available.

### Things to check first when it fails to compile

1. `kerberosAuthProvider.cpp` — SSPI is the most likely source of trouble.
   `InitializeSecurityContextA` takes a non-const `LPSTR` SPN, which is cast
   away from `std::string::c_str()`; MSVC may warn or error depending on the
   `/permissive-` setting. `NEGOSSP_NAME_A` is `#define`d defensively if the SDK
   does not provide it.
2. `tokenAuthProvider.cpp` — `_popen`/`_pclose` need `<stdio.h>`; `<io.h>` is
   included as well because some SDK versions declare the process helpers there.
   Both are MSVC-only, which is fine for this driver.
3. `setConnectAttr.cpp` — `SQL_ATTR_UID`/`SQL_ATTR_PASSWORD` are `#ifndef`
   guarded with the spec values 60/61. If the SDK defines them, the guard is
   inert; if the values ever disagree with the SDK, the SDK wins at compile time
   but the `case` labels would collide. Watch for duplicate-case errors.
4. `hostEnvironment.cpp` — `ProcessIdToSessionId` is in `kernel32`, which is
   linked by default. `wtsapi32` was deliberately *not* added to
   `target_link_libraries` because it is not needed; if a future change switches
   to `WTSGetActiveConsoleSessionId`, that library becomes required.
5. `dsnConfigForm.cpp` — the two-column layout uses hard-coded geometry
   constants. Nothing here should break compilation, but the visual result
   genuinely needs a human to look at it.

## Design decisions and why

### Why the ClickHouse driver was the model

The task was "make the OSS driver usable from the Power BI on-premises data
gateway." The ClickHouse OSS ODBC driver is the driver people actually get
working in that scenario, so its parameter surface is the one an administrator
already knows: `UID`/`PWD` aliases, a CA bundle path, a TLS-verify toggle, a
request timeout, and a client-identification string. Matching those names means
a DSN copied from a ClickHouse working example behaves the same way here, and it
avoids inventing a vocabulary that Power BI documentation will never use.

What was *not* copied: ClickHouse's `SSLMode` enum. A boolean `SSLVerify` plus a
separate `SSLCAInfo` path is simpler and maps more directly onto the curl
options the driver actually sets.

### Why session-0 detection instead of a config flag

The obvious approach is a `RequireInteractiveDesktop` DSN option. It was
rejected: a DSN is shared between whoever authors the report in Power BI Desktop
(interactive) and the gateway that runs it (session 0), so a static flag is
either wrong for the author or wrong for the gateway. Detecting the host at
runtime means the same DSN gives a good error in the gateway and keeps working
on the desktop.

The detection is deliberately biased toward "interactive" — if
`ProcessIdToSessionId` fails, the driver assumes it *can* show UI. Wrongly
refusing to run a working desktop configuration is worse than letting an
interactive flow hang in a service, because the hang at least produces a
timeout that the administrator can investigate.

### Why `ConnectionSetup` replaced positional arguments

The old constructor took twelve positional parameters, most of them
`std::string`. Adding five more options made a transposition bug essentially
inevitable and completely silent. The struct is the cheap fix.

### Why secrets go through DPAPI rather than the credential manager

`ClientSecret` was already DPAPI-encrypted in `ODBC.INI`, so `Password` and
`Token` follow the existing pattern rather than introducing a second storage
mechanism. `configDSN.cpp` was generalised from a single hard-coded key to a
`SECRET_KEYS` list for the same reason.

The limitation is worth stating plainly: `CRYPTPROTECT_LOCAL_MACHINE` (used for
system DSNs) makes a secret readable by *any* process on that host. That is
acceptable for a gateway machine only because the same is already true of every
other gateway credential on it. It is not a property of this design — it is
inherited from the pre-existing one.

### Why `X-Trino-User` is derived from token claims

Trino requires `X-Trino-User` on every request and rejects a value that does not
match the principal in the presented credential. The OIDC providers never set
it, which means they only ever worked against deployments that did not enforce
the check. Deriving it from `preferred_username` → `sub` → `email` mirrors what
the server does when it resolves the token, so the two agree by construction.

An explicitly configured `Username` still wins, because some deployments put
the end user in a custom claim and want to set it by hand.

### Why `refresh()` must never throw

`ConnectionConfig::getCurl()` calls `refresh()` from underneath ordinary ODBC
entry points. An exception escaping there crosses the C ABI and terminates the
host process — in production that is "Power BI Desktop vanished," which is
close to the worst possible failure mode for a driver. So:

- Kerberos records the failure and lets the request go out unauthenticated, so
  Trino's 401 becomes a normal `SQL_ERROR`.
- The token provider catches around `loadToken()` for the same reason.
- `SQLConnect` wraps `configure()` in a try/catch and reports through
  `SQLGetDiagRec`.

**Any new auth provider must follow this rule.** It is the single most
important constraint on the provider interface and it is not enforced by the
type system.

### The `getCurl()` refresh loop

```cpp
if (this->authConfigPtr->isExpired()) {
  this->authConfigPtr->refresh(...);
  goto curlSetup;   // re-checks isExpired()
}
```

`isExpired()` returning true after a failed `refresh()` is an infinite loop.
Every provider therefore needs a guard that survives a failed refresh:

- `TokenAuthConfig`: `refreshAttempted`, plus a separate `tokenIsExpired()` for
  the "did the new token actually work?" question. Using `isExpired()` there is
  the bug that was fixed — it always reports false once the guard is set, so the
  guard was cleared every time.
- `KerberosAuthConfig`: `handshakeFailed`.
- `BasicAuthConfig` / `NoAuthAuthConfig`: `isExpired()` is constant `false`, so
  there is nothing to guard.

## Security posture

- `SSLVerify=false` is the only way to weaken TLS, and it logs a `WARN` every
  connection. It exists for self-signed coordinators; it should not be the
  default posture on a gateway.
- `TokenCommand` executes a command line under the host process's service
  account. Anyone who can edit the DSN can therefore run code as the gateway
  service account. System DSNs should be administratively locked down
  accordingly.
- Basic auth over HTTP is rejected by Trino, so a misconfigured DSN fails rather
  than leaking a credential in cleartext.
- The Kerberos handshake POSTs a throwaway query (`SELECT 'authenticating...'`)
  to `v1/statement` twice per authentication. It is cheap, but it is real server
  work; the resulting token is cached for the life of the connection.

## Known gaps

These are deliberate omissions, not oversights.

- **No Kerberos credential delegation.** The handshake authenticates the
  gateway's service account. Impersonating the Power BI end user requires
  protocol transition / constrained delegation configured on the AD side, which
  the driver cannot do from here. `Username` is therefore an assertion made by
  the gateway, and Trino must be configured to accept that assertion.
- **No token revocation or refresh-token rotation** for `Token Auth`. When the
  token expires, the source is re-read; that is all.
- **No `SQLBrowseConnect`.** The new options are reachable from the DSN dialog
  and the connection string, but not through iterative discovery.
- **The DSN form has no scroll and no validation.** If more fields are added,
  the fixed geometry will need to become a layout manager. Port numbers and
  timeouts are not validated until `configure()` runs.
- **Tests were not extended.** The suite is DSN-based (`TrinoTestDebug` /
  `TrinoTestRelease`) and assumes a vanilla Trino with the insecure
  authenticator. The `TestUser` default was preserved specifically so it keeps
  passing. There is no test coverage for any of the new auth methods.

## Verification checklist

Before this is believed, at minimum:

1. Build for x64 with the repository's vcpkg manifest.
2. `No Auth` against a stock Trino container — confirms the `ConnectionSetup`
   refactor and the `X-Trino-User` change did not regress the existing path.
3. `TestDriver.exe` full suite.
4. `Basic Auth` against Trino with `password-authenticator.enabled=true` over
   HTTPS, and confirm it *fails* over HTTP.
5. `Token Auth` with a hand-written JWT (HS256, `sub` claim, no `Username`) —
   confirms claim-derived `X-Trino-User`.
6. `Token Auth` with `TokenCommand` returning an expired token — must produce a
   401 error, **not** a hang. This is the loop-guard regression test.
7. `Kerberos Auth` on a domain-joined machine against Trino with the Kerberos
   authenticator, both with and without an explicit `ServicePrincipalName`.
8. `SQLGetInfo(SQL_USER_NAME)` returns the expected identity for each method.
9. Save a DSN with a password and token, then inspect `ODBC.INI`: both must be
   `encrypted*` keys, no plaintext.
10. Set `AuthMethod=External Auth` on a system DSN and connect from a Windows
    service — must fail fast with the actionable message, not hang.
11. Confirm the Kerberos path leaves the connection usable for ordinary queries
    (this is the `CURLOPT_POSTFIELDS` regression).
12. Open the DSN dialog for each of the seven auth methods and confirm the
    greyed-out fields are the right ones, and that untouched fields save as
    empty rather than as `<Not Required>`.
