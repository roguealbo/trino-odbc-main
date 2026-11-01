#include "tokenAuthProvider.hpp"

#include <cstdio>
#include <fstream>
#include <map>
#include <stdexcept>
#include <sstream>
#include <string>

/*
MSVC declares _popen and _pclose in <stdio.h>; <io.h> is included as well
because some Windows SDK versions surface the process helpers there instead.
Including both is harmless and keeps the build working across SDK releases.
*/
#include <io.h>
#include <stdio.h>

#include <nlohmann/json.hpp>

#include "../../util/cryptUtils.hpp"
#include "../../util/timeUtils.hpp"
#include "../../util/windowsLean.hpp"
#include "../../util/writeLog.hpp"
#include "tokens/tokenParser.hpp"

using json = nlohmann::json;

/*
A JWT that is within this many seconds of expiry is treated as already
expired so that a token about to die mid-query is replaced proactively.
*/
static const long long TOKEN_EXPIRY_GRACE_PERIOD_S = 60 * 10;

/*
Read the whole contents of a text file as a string. Throws if the file cannot
be opened, so that a misconfigured tokenFile surfaces as a clear error rather
than a silent 401.
*/
static std::string readFileContents(const std::string& path) {
  std::ifstream inputFile(path);
  if (!inputFile.is_open()) {
    throw std::runtime_error("Could not open token file: " + path);
  }
  std::ostringstream contents;
  contents << inputFile.rdbuf();
  return contents.str();
}

/*
Run a command line and return its standard output.

Used for the tokenCommand option, which lets an administrator fetch a fresh
JWT from a secret store or cloud CLI at connection time. The command runs
under the service account of the host process, which for the Power BI
on-premises data gateway is the gateway service account.
*/
static std::string runCommand(const std::string& command) {
  if (command.empty()) {
    return "";
  }

  FILE* pipe = _popen(command.c_str(), "r");
  if (pipe == nullptr) {
    throw std::runtime_error("Failed to launch token command: " + command);
  }

  std::ostringstream output;
  char buffer[4096];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output << buffer;
  }

  int exitCode = _pclose(pipe);
  if (exitCode != 0) {
    throw std::runtime_error("Token command exited with code " +
                             std::to_string(exitCode) + ": " + command);
  }

  std::string result = output.str();
  // Commands tend to emit a trailing newline; strip trailing whitespace.
  size_t lastNonWs = result.find_last_not_of(" \t\r\n");
  if (lastNonWs == std::string::npos) {
    return "";
  }
  return result.substr(0, lastNonWs + 1);
}

class TokenAuthConfig : public AuthConfig {
  private:
    std::string username;
    std::string token;
    std::string tokenFile;
    std::string tokenCommand;
    json parsedAccessToken;

    /*
    Guards against an infinite refresh loop. ConnectionConfig::getCurl() calls
    refresh() and then re-checks isExpired() in a loop, so a token that cannot
    be renewed (an expired static token, or a failing tokenCommand) must stop
    reporting itself as expired after one attempt. The request then goes out
    unauthenticated and Trino returns a proper 401 that the driver can surface
    as a real error instead of the driver spinning forever.
    */
    bool refreshAttempted = false;

    void applyToken() {
      this->headers["Authorization"] = "Bearer " + this->token;
      this->applyUserHeader();
    }

    /*
    Trino requires X-Trino-User on every request and rejects a value that does
    not match the principal in the presented token. Prefer the configured
    username; otherwise take the token's preferred_username or sub claim, which
    is what Trino's JWT authenticator resolves anyway.

    Called from applyToken() rather than only from the constructor, so a
    refreshed token that carries different claims also updates the identity
    being asserted.
    */
    void applyUserHeader() {
      std::string resolvedUser = this->username;
      if (resolvedUser.empty()) {
        resolvedUser = this->parsedAccessToken.value<std::string>(
            "preferred_username",
            this->parsedAccessToken.value<std::string>("sub", ""));
      }
      if (resolvedUser.empty()) {
        /*
        Only fatal at construction time, when there is no header at all to send.
        During a refresh the previous header is still in place, so log and keep
        it rather than throwing out of ConnectionConfig::getCurl().
        */
        if (this->headers.count("X-Trino-User") == 0) {
          throw std::invalid_argument(
              "JWT authentication requires a username: set one in the DSN or "
              "use a token with a preferred_username or sub claim");
        }
        WriteLog(LL_WARN,
                 "  Refreshed JWT has no username claim; keeping the previous "
                 "X-Trino-User value.");
        return;
      }
      this->trinoUser               = resolvedUser;
      this->headers["X-Trino-User"] = resolvedUser;
    }

    void setToken(std::string newToken) {
      this->token = newToken;
      if (not this->token.empty()) {
        this->parsedAccessToken = parseAccessToken(this->token);
      } else {
        this->parsedAccessToken = json(json::value_t::object);
      }
    }

    /*
    Expiry of the current token, ignoring the refresh guard.

    isExpired() deliberately lies once a refresh has been attempted (see the
    comment there) so ConnectionConfig::getCurl() cannot spin forever. That
    makes it unusable for asking "did the token I just loaded actually work?",
    so refresh() uses this guard-free version instead.
    */
    bool tokenIsExpired() {
      if (this->token.empty()) {
        return true;
      }
      // A token with no exp claim (e.g. an opaque token) is treated as
      // permanently valid; the server is the authority on it.
      long long expiresAt = this->parsedAccessToken.value<long long>("exp", 0LL);
      if (expiresAt == 0LL) {
        return false;
      }
      long long timeToExpiry = expiresAt - getSecondsSinceEpoch();
      return (timeToExpiry - TOKEN_EXPIRY_GRACE_PERIOD_S) < 0;
    }

    /*
    Obtain the token from whichever source the DSN configured. Returns an empty
    string if the source produced nothing.
    */
    std::string loadToken() {
      if (not this->tokenFile.empty()) {
        WriteLog(LL_INFO, "  Reading JWT from token file: " + this->tokenFile);
        return readFileContents(this->tokenFile);
      }
      if (not this->tokenCommand.empty()) {
        WriteLog(LL_INFO, "  Obtaining JWT from token command");
        return runCommand(this->tokenCommand);
      }
      // A static token supplied directly in the config.
      return this->token;
    }

  public:
    TokenAuthConfig(std::string hostname,
                    unsigned short port,
                    std::string connectionName,
                    std::string username,
                    std::string token,
                    std::string tokenFile,
                    std::string tokenCommand)
        : AuthConfig(hostname, port, connectionName) {
      this->username     = username;
      this->tokenFile    = tokenFile;
      this->tokenCommand = tokenCommand;

      if (token.empty() && tokenFile.empty() && tokenCommand.empty()) {
        throw std::invalid_argument(
            "JWT authentication requires one of token, tokenFile, or "
            "tokenCommand");
      }

      // Trino requires X-Trino-User on every request. Prefer the configured
      // username; otherwise fall back to the token's preferred_username or
      // sub claim, which is what Trino's JWT authenticator resolves anyway.
      setToken(loadToken());
      this->applyToken();
      WriteLog(LL_INFO, "  JWT authentication configured for user: " +
                            this->trinoUser);
    }

    bool const isExpired() override {
      if (this->refreshAttempted) {
        /*
        A refresh has already been attempted and the token is still expired.
        ConnectionConfig::getCurl() loops while isExpired() is true, so we must
        stop reporting expiry here or the driver would spin forever. The
        request goes out with the stale token and Trino returns a proper 401.
        */
        return false;
      }
      return this->tokenIsExpired();
    }

    void
    refresh(CURL* curl,
            std::string* responseData,
            std::map<std::string, std::string>* responseHeaderData) override {
      if (this->refreshAttempted) {
        return;
      }
      this->refreshAttempted = true;

      WriteLog(LL_TRACE, "  Refreshing JWT");
      std::string newToken;
      try {
        newToken = loadToken();
      } catch (const std::exception& e) {
        WriteLog(LL_ERROR,
                 std::string("  Failed to obtain JWT: ") + e.what());
        return;
      }

      if (newToken.empty()) {
        WriteLog(LL_ERROR, "  JWT source produced an empty token");
        return;
      }

      setToken(newToken);
      this->applyToken();

      /*
      Note: this must call tokenIsExpired() and not isExpired(). isExpired()
      reports "not expired" whenever refreshAttempted is set, which it always is
      at this point, so using it here would clear the guard on every attempt and
      put ConnectionConfig::getCurl() straight back into its refresh loop.
      */
      if (not this->tokenIsExpired()) {
        /*
        The token was renewed successfully. Clear the guard so a later expiry
        (for example a rotated token file) can trigger another refresh.
        */
        this->refreshAttempted = false;
        WriteLog(LL_INFO, "  JWT authentication token applied");
      } else {
        WriteLog(LL_WARN,
                 "  JWT source produced an already-expired token; the request "
                 "will proceed and the server will report the failure.");
      }
    }

    ~TokenAuthConfig() override = default;
};

std::unique_ptr<AuthConfig> getTokenAuthConfigPtr(std::string hostname,
                                                  unsigned short port,
                                                  std::string connectionName,
                                                  std::string username,
                                                  std::string token,
                                                  std::string tokenFile,
                                                  std::string tokenCommand) {
  return std::make_unique<TokenAuthConfig>(hostname,
                                           port,
                                           connectionName,
                                           username,
                                           token,
                                           tokenFile,
                                           tokenCommand);
}
