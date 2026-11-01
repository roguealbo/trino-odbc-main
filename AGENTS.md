# AGENTS.md

Instructions for AI agents and new contributors continuing work on this
driver. Read `notes.md` first for the current state and `CHANGELOG.md` for what
has already changed.

## What this project is

An open-source (Apache-2.0) ODBC driver for Trino, targeting Microsoft Excel
and Power BI on Windows. It is a partial implementation of the ODBC core
specification, built with CMake + vcpkg, MSVC-only, C++20.

## The one thing to understand before touching authentication

**`AuthConfig::refresh()` must never throw, and `isExpired()` must never return
true after a failed `refresh()`.**

`ConnectionConfig::getCurl()` does this:

```cpp
if (this->authConfigPtr->isExpired()) {
  this->authConfigPtr->refresh(this->curl, ...);
  goto curlSetup;   // loops back and re-checks isExpired()
}
```

Two failure modes follow from that, and both are catastrophic:

- A provider whose `isExpired()` stays true after `refresh()` spins forever.
  Every provider needs a guard flag that survives a failed refresh
  (`refreshAttempted`, `handshakeFailed`, ...).
- An exception escaping `refresh()` crosses the C ABI and kills the host
  process — "Power BI Desktop vanished," with no diagnostic.

Record failures and let the request go out unauthenticated. Trino's 401 becomes
a normal `SQL_ERROR` that the application can display. This is not enforced by
the type system; it is enforced by reading this paragraph.

If you need to ask "did the credential I just loaded actually work?", do **not**
call `isExpired()` — it lies once the guard is set. Add a guard-free helper
(see `TokenAuthConfig::tokenIsExpired()`).

## Architecture and layering

Five directories, and the dependency direction matters:

* `src/driver` — ODBC API implementation. One file per entry point.
* `src/driver/config` — DSN/driver-manager configuration, including the Win32
  DSN dialog.
* `src/trinoAPIWrapper` — C++ wrapper over Trino's REST API plus the auth layer.
  **Must not include `sql.h` or `sqlext.h`.** Avoid `windows.h` where possible.
* `src/util` — generic helpers shared by both layers.
* `test` — GoogleTest suite.

`src/driver` may depend on `src/trinoAPIWrapper`, never the reverse. This is why
`AuthCredentials` and `ConnectionOptions` are plain structs in the wrapper layer
rather than reusing `DriverConfig`: the wrapper must stay free of ODBC types.

### Adding a new authentication method

The established pattern, in order:

1. Add an enumerator to `src/trinoAPIWrapper/apiAuthMethod.hpp` and classify it
   in `isNonInteractiveAuthMethod()`. Getting this wrong makes the method
   unusable from a gateway (or makes an interactive method hang there).
2. Create `authProvider/<name>AuthProvider.{hpp,cpp}` exposing
   `std::unique_ptr<AuthConfig> get<Name>AuthConfigPtr(...)`. Follow
   `basicAuthProvider.cpp` as the smallest complete example.
3. Set `this->trinoUser` and `this->headers["X-Trino-User"]`. Trino requires
   that header on every request and rejects a mismatch with the credential's
   principal.
4. Handle the new case in `ConnectionConfig`'s switch in
   `connectionConfig.cpp`, passing fields from `ConnectionSetup`.
5. Add the display name to `AUTH_METHOD_NAMES`, `AUTH_METHOD_VALUES`, and
   `AUTH_METHOD_TO_AUTH_NAME` in `driverConfig.cpp`.
6. Add it to `AUTH_NAME_TO_AUTH_METHOD` — **keyed by the result of
   `capitalizedCase()` on the display name.** That helper lowercases everything
   and then uppercases the first letter of each space-delimited word, so
   `"OIDC Client Cred Auth"` must be stored as `"Oidc Client Cred Auth"`.
   Getting this wrong produces a `std::out_of_range` at DSN read time with a
   confusing message.
7. Add defaults to `DRIVER_CONFIG_DEFAULT_VALUES` (all lowercase keys), new
   accessors on `DriverConfig`, `applyKVP` calls in `driverConfigFromKVPs`, and
   non-empty guards in `driverConfigToKVPs`.
8. Read the new keys in `profileReader.cpp`. If a key holds a secret, add it to
   `SECRET_KEYS` in `configDSN.cpp` and decrypt `encrypted<Key>` in the reader.
9. Add fields to `dsnConfigForm.cpp`: an `ID_EDIT_*`/`ID_STATIC_*` pair, a label,
   a control, `setEditText` pre-population, a `readField` call on save, and the
   field ID in `fieldsForAuthMethod()` plus `ALL_CREDENTIAL_FIELDS`.
10. Add the source file to `CMakeLists.txt`.

### Adding a new connection parameter

`DriverConfig` field + accessors → `DRIVER_CONFIG_DEFAULT_VALUES` → `applyKVP`
in `driverConfigFromKVPs` → `driverConfigToKVPs` → `profileReader.cpp` → DSN
form field. Then document it in the README parameter tables.

Two traps:

- `applyKVP` looks up the camelCase key and then the lowercase key, and the
  **lowercase (connection-string) value wins**. That ordering is intentional.
- `applyKVP` skips empty values so that `port=;` does not throw in `std::stoi`.
  Preserve that if you rewrite it.
- `driverConfigToKVPs` writes **every setting unconditionally, including empty
  values**, because it feeds `ConfigDSN`'s loop that persists the whole form. If
  an empty setting were skipped, clearing a field in the dialog would leave the
  previous value in `ODBC.INI` — deleting a password would leave the old
  encrypted password in place and still in use. Only `dsn` and `driver` are
  conditional, because they identify the entry rather than configure it.
  This is safe for the other caller, `SQLDriverConnect`, which uses the same
  function to fill DSN defaults, because `driverConfigFromKVPs` ignores empty
  values. **Keep those two rules paired**: if you make `driverConfigFromKVPs`
  start honouring empty values, you must simultaneously make
  `driverConfigToKVPs` skip them, or a DSN with a blank field will start wiping
  stored values.

## Non-interactive / gateway constraints

The Power BI on-premises data gateway runs as a Windows service in session 0
with no desktop. Anything that opens a browser or waits on a human hangs until
timeout there.

- Interactive methods are rejected at connect time via
  `isInteractiveDesktopAvailable()` in `src/util/hostEnvironment.cpp`. Do not
  replace this with a DSN flag: the same DSN is used interactively by the report
  author and headlessly by the gateway, so a static flag is wrong for one of
  them.
- The detection is biased toward "interactive" when it cannot determine the
  session. Keep that bias — wrongly refusing a working desktop configuration is
  worse than a hang that at least produces a timeout.
- Credentials arriving from the application (`SQLConnect` arguments,
  `SQL_ATTR_UID`/`SQL_ATTR_PASSWORD`) must override DSN values, and an *empty*
  argument means "not supplied" — it must not clear a stored value. All of it
  funnels through `Connection::configure()` via `ATTR_UserName` /
  `ATTR_Password` / `ATTR_CredentialsProvided`; keep that single precedence
  path.

## Coding conventions

`CONTRIBUTING.md` is authoritative. The parts most likely to be violated by an
LLM writing C++ in this repo:

- **No member initializer lists.** Assign in the constructor body with `this->`.
- **Always use braces**, even for single-statement `if`/`for`/`case`.
- **No `typedef` aliases**; use `auto` or spell out the type.
- **Avoid `const`, `static`, and `&`-references** unless there is a demonstrable
  need. This is deliberate: the maintainers prefer readability over idiom.
- Use `static_cast` / `reinterpret_cast`, never C-style casts. (The SSPI calls
  do cast away const from `LPSTR`; that is a documented exception forced by the
  Win32 API signature.)
- camelCase variables, CapitalCase classes, `this->` for members.
- Formatting is enforced by clang-format (`.clang-format`, LLVM base with
  `BinPackParameters: false`, `IndentAccessModifiers: true`, 2-space indent).
  Run clang-format on every file you touch and remove trailing whitespace.
- Comment blocks explain *why*, including the failure being avoided. Match that
  density — the existing code comments are unusually explanatory and new code
  that is silent about its reasoning sticks out.

## Build and test

```
# Windows, from a Developer Command Prompt, after bootstrapping vcpkg
cmake --preset <preset>        # see CMakePresets.json
cmake --build build/<preset>
```

Dependencies come from `vcpkg.json`: `curl[openssl]`, `nlohmann-json`, `gtest`.
Linked system libraries: `odbccp32`, `crypt32`, `secur32`,
`legacy_stdio_definitions`.

The test suite is **DSN-based and integration-only** — it needs a live Trino and
a registered DSN named `TrinoTestDebug` or `TrinoTestRelease` (chosen by the
`DEBUG` define). It assumes a vanilla Trino with the insecure authenticator and
the `No Auth` default user `TestUser`. Do not change that default without
updating the tests.

There is currently **no unit-test coverage for any auth method**. Adding one
means either a fake `AuthConfig` driving `ConnectionConfig` against a local HTTP
stub, or extracting the token/claim logic so it is testable without a network.
That is the highest-value missing test in the repo.

Verify changes against the checklist in `notes.md`.

## Environment caveat

The changes described in `CHANGELOG.md` were authored on Linux with **no
compiler available** (no `g++`, `clang++`, `cmake`, or mingw) and have never
been compiled. The driver is MSVC-only. Before extending this work, get a clean
build first; the SSPI and ODBC-3.8-attribute code is the most likely to need
correction. See "Things to check first when it fails to compile" in `notes.md`.

## Suggested future work

In rough priority order.

### Must do before shipping

1. **Compile and fix.** Nothing here is build-verified.
2. **Test the loop guards.** A `TokenCommand` that returns an expired token must
   produce a 401 error, not a hang. This is the regression that the
   `tokenIsExpired()` fix addresses and it is currently untested.
3. **Test the Kerberos handle restore.** Confirm ordinary queries work after a
   Kerberos handshake — this catches the `CURLOPT_POSTFIELDS` leak.
4. **Add auth-method tests**, at least for the claim-derived `X-Trino-User`
   logic, which is pure and easily unit-tested.

### High value

5. **Kerberos credential caching across connections.** Each `ConnectionConfig`
   builds its own SSPI context. A gateway with many datasources repeats the
   handshake per connection.
6. **Honour `SQL_ATTR_LOGIN_TIMEOUT`.** It is stored in `ATTR_LoginTimeout` and
   never used. The gateway sets it.
7. **`SSLCAInfo` directory support.** `CURLOPT_CAINFO` takes a file; a directory
   needs `CURLOPT_CAPATH`. The README currently promises "file or directory".
8. **Validate DSN input in the form** rather than failing later in `configure()`
   — port range, timeout is numeric, `SSLVerify` is a recognised word.
9. **`SQLGetInfo(SQL_DATA_SOURCE_NAME)`** and `SQL_CATALOG_NAME` from the
   connection string. Power BI's gateway queries more of `SQLGetInfo` than
   Power BI Desktop does, and an unhandled `InfoType` currently returns
   `SQL_ERROR` with `HY091`, which some clients treat as fatal. Consider
   returning `SQL_SUCCESS` with an empty value for unknown-but-harmless info
   types.

### Worth considering

10. **`AuthMethod=mTLS`** — certificate authentication is common for service
    accounts and needs only `CURLOPT_SSLCERT`/`CURLOPT_SSLKEY`.
11. **OIDC refresh-token grant** so a long-running import can renew an
    authorization-code token without a browser.
12. **A `Proxy` parameter.** Gateways frequently sit behind a corporate proxy;
    curl needs `CURLOPT_PROXY`. This is a likely blocker for real gateway
    deployments and is not yet handled.
13. **Scrollable or resizable DSN form** before more fields are added; the
    current geometry is hard-coded constants.
14. **Structured error mapping.** Trino's JSON error body is currently
    discarded on non-2xx responses. Surfacing its message in `SQLGetDiagRec`
    would make gateway troubleshooting dramatically easier, since the gateway
    shows only the driver's diagnostic text.

### Explicitly out of scope

- Reproducing Starburst's commercial Trino ODBC driver. It is a proprietary
  licensed product; its features can be *equivalenced* independently, but its
  code and behaviour cannot be copied.
- Anything requiring the ODBC wide-char (UCS-2) path, which the driver does not
  implement.

## Files most likely to need attention

| File | Why |
| --- | --- |
| `src/trinoAPIWrapper/connectionConfig.cpp` | Every auth method and every curl option funnels through here. |
| `src/trinoAPIWrapper/authProvider/*` | The loop-safety and never-throw rules above. |
| `src/driver/config/driverConfig.cpp` | Four parallel maps plus defaults; easy to update three of five. |
| `src/driver/config/dsnConfigForm.cpp` | Hard-coded layout geometry, placeholder semantics. |
| `src/driver/getInfo.cpp` | Unknown info types return `SQL_ERROR`; gateway clients ask for more than desktop ones. |
| `CMakeLists.txt` | New sources must be listed explicitly; there is no glob. |
