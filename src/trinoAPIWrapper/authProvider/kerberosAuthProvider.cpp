#include "kerberosAuthProvider.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../../util/cryptUtils.hpp"
#include "../../util/timeUtils.hpp"
#include "../../util/windowsLean.hpp"
#include "../../util/writeLog.hpp"

#include <sspi.h>

#ifndef NEGOSSP_NAME_A
#define NEGOSSP_NAME_A "Negotiate"
#endif

/*
Kerberos tickets are typically issued for 10 hours. Re-authenticate a little
early so a long-running gateway refresh never uses a ticket that dies
mid-query.
*/
static const long long KERBEROS_REAUTH_GRACE_PERIOD_S = 60 * 10;

/*
A trigger query used to provoke the 401 Negotiate challenge from the
coordinator. It is cheap and produces no meaningful result set.
*/
static const std::string KERBEROS_TRIGGER_ENDPOINT = "v1/statement";
static const std::string KERBEROS_TRIGGER_QUERY    = "SELECT 'authenticating...'";

/*
Owns the SSPI credential and security context handles for one connection and
performs the HTTP Negotiate handshake.

The handshake follows the same shape as the external-auth provider: the driver
POSTs a throwaway query with no credentials, reads the WWW-Authenticate
Negotiate challenge, and answers it using a service ticket obtained from the
operating system. Once established, the resulting token is reused for every
request until the ticket expires.
*/
class KerberosAuthConfig : public AuthConfig {
  private:
    std::string username;
    std::string spn;
    bool authenticated        = false;
    bool refreshAttempted     = false;
    bool handshakeFailed      = false;
    long long authenticatedAt = 0;

    CredHandle credHandle;
    CtxtHandle contextHandle;
    bool credHandleAcquired    = false;
    bool contextHandleAcquired = false;

    void releaseHandles() {
      if (this->contextHandleAcquired) {
        DeleteSecurityContext(&this->contextHandle);
        this->contextHandleAcquired = false;
      }
      if (this->credHandleAcquired) {
        FreeCredentialsHandle(&this->credHandle);
        this->credHandleAcquired = false;
      }
    }

    void acquireCredentials() {
      if (this->credHandleAcquired) {
        return;
      }

      /*
      The handshake runs as the process token, i.e. the account the host
      application runs under. For the Power BI on-premises data gateway that
      is the gateway service account, which must be domain-joined and able to
      obtain a ticket for the Trino service principal (directly, or via
      protocol transition/Resource-Based Constrained Delegation configured on
      the AD side).
      */
      SECURITY_STATUS status = AcquireCredentialsHandleA(
          nullptr,
          (LPSTR)NEGOSSP_NAME_A,
          SECPKG_CRED_OUTBOUND,
          nullptr,
          nullptr,
          nullptr,
          nullptr,
          &this->credHandle,
          nullptr);

      if (status != SEC_E_OK) {
        throw std::runtime_error("AcquireCredentialsHandle failed with status " +
                                 std::to_string((unsigned long)status));
      }
      this->credHandleAcquired = true;
    }

    /*
    Drive one round of InitializeSecurityContext and return the output token
    as base64, ready to place in an Authorization header. An empty return value
    means the context is complete and no further token is required.
    */
    std::string stepContext(const std::string& serverToken) {
      this->acquireCredentials();

      SecBufferDesc inputDesc;
      SecBuffer inputBuffer;
      SecBufferDesc outputDesc;
      SecBuffer outputBuffer;

      std::vector<unsigned char> serverTokenBytes;
      if (not serverToken.empty()) {
        std::string decoded = base64Decode(serverToken);
        serverTokenBytes.assign(decoded.begin(), decoded.end());
      }

      ZeroMemory(&inputDesc, sizeof(inputDesc));
      ZeroMemory(&inputBuffer, sizeof(inputBuffer));
      inputDesc.ulVersion = SECBUFFER_VERSION;
      inputDesc.cBuffers  = 1;
      inputDesc.pBuffers  = &inputBuffer;
      inputBuffer.BufferType = SECBUFFER_TOKEN;
      inputBuffer.cbBuffer =
          static_cast<unsigned long>(serverTokenBytes.size());
      inputBuffer.pvBuffer = serverTokenBytes.empty()
                                 ? nullptr
                                 : static_cast<void*>(serverTokenBytes.data());

      ZeroMemory(&outputDesc, sizeof(outputDesc));
      ZeroMemory(&outputBuffer, sizeof(outputBuffer));
      outputDesc.ulVersion = SECBUFFER_VERSION;
      outputDesc.cBuffers  = 1;
      outputDesc.pBuffers  = &outputBuffer;
      outputBuffer.BufferType = SECBUFFER_TOKEN;
      outputBuffer.cbBuffer   = 0;
      outputBuffer.pvBuffer   = nullptr;

      ULONG contextReq = ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_MUTUAL_AUTH |
                         ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT;

      SECURITY_STATUS status;
      TimeStamp expiry = {0, 0};

      if (this->contextHandleAcquired) {
        status = InitializeSecurityContextA(&this->credHandle,
                                            &this->contextHandle,
                                            (LPSTR)this->spn.c_str(),
                                            contextReq,
                                            0,
                                            SECURITY_NATIVE_DREP,
                                            &inputDesc,
                                            0,
                                            &this->contextHandle,
                                            &outputDesc,
                                            nullptr,
                                            &expiry);
      } else {
        status = InitializeSecurityContextA(&this->credHandle,
                                            nullptr,
                                            (LPSTR)this->spn.c_str(),
                                            contextReq,
                                            0,
                                            SECURITY_NATIVE_DREP,
                                            &inputDesc,
                                            0,
                                            &this->contextHandle,
                                            &outputDesc,
                                            nullptr,
                                            &expiry);
        if (status == SEC_E_OK or status == SEC_I_CONTINUE_NEEDED) {
          this->contextHandleAcquired = true;
        }
      }

      if (status != SEC_E_OK and status != SEC_I_CONTINUE_NEEDED) {
        std::string message =
            "InitializeSecurityContext failed with status " +
            std::to_string((unsigned long)status);
        if (outputBuffer.pvBuffer != nullptr) {
          FreeContextBuffer(outputBuffer.pvBuffer);
        }
        throw std::runtime_error(message);
      }

      std::string token;
      if (outputBuffer.cbBuffer > 0 and outputBuffer.pvBuffer != nullptr) {
        const unsigned char* bytes =
            static_cast<const unsigned char*>(outputBuffer.pvBuffer);
        token = base64Encode(std::string(bytes, bytes + outputBuffer.cbBuffer));
        FreeContextBuffer(outputBuffer.pvBuffer);
      }

      // A ticket lifetime is reported by SSPI; remember when we authenticated
      // so isExpired() can proactively re-run the handshake.
      if (status == SEC_E_OK) {
        this->authenticatedAt = getSecondsSinceEpoch();
      }

      return token;
    }

    /*
    POST the trigger query with a given Authorization value and report the
    resulting HTTP status plus any Negotiate challenge the server returned.
    */
    struct ChallengeResult {
        long httpStatus;
        std::string negotiateChallenge;
    };

    ChallengeResult sendWithAuth(CURL* curl,
                                 std::string* responseData,
                                 std::map<std::string, std::string>* headerData,
                                 const std::string& authorizationValue) {
      responseData->clear();
      headerData->clear();

      std::string url = this->hostname + ":" + std::to_string(this->port) +
                        "/" + KERBEROS_TRIGGER_ENDPOINT;

      struct curl_slist* headers = nullptr;
      headers = curl_slist_append(headers,
                                  ("Authorization: " + authorizationValue).c_str());
      if (not this->username.empty()) {
        headers = curl_slist_append(
            headers, ("X-Trino-User: " + this->username).c_str());
      }

      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(
          curl, CURLOPT_POSTFIELDS, KERBEROS_TRIGGER_QUERY.c_str());

      CURLcode res = curl_easy_perform(curl);
      if (res != CURLE_OK) {
        WriteLog(LL_ERROR,
                 "  Kerberos trigger request failed: " +
                     std::string(curl_easy_strerror(res)));
      }

      long httpStatus = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);

      /*
      Put the handle back the way getCurl() expects to find it. The handshake
      borrowed a handle that the rest of the driver assumes is set up for GET;
      leaving CURLOPT_POSTFIELDS in place would silently turn the next
      statement poll into a POST with the trigger query as its body.
      */
      curl_easy_setopt(curl, CURLOPT_POST, 0L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, nullptr);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
      curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

      ChallengeResult result;
      result.httpStatus = httpStatus;

      // The driver's header callback lower-cases names and joins repeated
      // headers with newlines, so scan for a Negotiate challenge.
      auto it = headerData->find("www-authenticate");
      if (it != headerData->end()) {
        const std::string prefix = "Negotiate ";
        size_t pos               = it->second.find(prefix);
        if (pos != std::string::npos) {
          size_t start = pos + prefix.size();
          size_t end   = it->second.find('\n', start);
          result.negotiateChallenge =
              it->second.substr(start, end == std::string::npos
                                            ? std::string::npos
                                            : end - start);
        }
      }

      curl_slist_free_all(headers);
      return result;
    }

  public:
    KerberosAuthConfig(std::string hostname,
                       unsigned short port,
                       std::string connectionName,
                       std::string username,
                       std::string servicePrincipalName)
        : AuthConfig(hostname, port, connectionName) {
      this->username = username;

      if (not servicePrincipalName.empty()) {
        this->spn = servicePrincipalName;
      } else {
        // Derive the principal from the host we are connecting to. Trino's
        // Kerberos authenticator registers its service as HTTP/<fqdn>.
        size_t slashPos = hostname.find("//");
        std::string host =
            slashPos == std::string::npos ? hostname : hostname.substr(slashPos + 2);
        size_t colonPos = host.find(':');
        if (colonPos != std::string::npos) {
          host = host.substr(0, colonPos);
        }
        this->spn = "HTTP/" + host;
      }

      ZeroMemory(&this->credHandle, sizeof(this->credHandle));
      ZeroMemory(&this->contextHandle, sizeof(this->contextHandle));

      if (this->username.empty()) {
        throw std::invalid_argument(
            "username is required for Kerberos authentication: Trino derives "
            "the end user from the Kerberos principal, and the X-Trino-User "
            "header must match it");
      }
      this->trinoUser                = this->username;
      this->headers.insert({"X-Trino-User", this->username});

      WriteLog(LL_INFO, "  Kerberos authentication configured for SPN: " +
                            this->spn);
    }

    bool const isExpired() override {
      if (this->handshakeFailed) {
        /*
        The handshake has already failed once. ConnectionConfig::getCurl()
        loops while isExpired() is true, so reporting expiry again here would
        spin forever. Let the request go out unauthenticated and let Trino
        return a real 401 that surfaces to the application.
        */
        return false;
      }
      if (not this->authenticated) {
        return true;
      }
      long long age = getSecondsSinceEpoch() - this->authenticatedAt;
      return age > (3600 * 10 - KERBEROS_REAUTH_GRACE_PERIOD_S);
    }

    void
    refresh(CURL* curl,
            std::string* responseData,
            std::map<std::string, std::string>* responseHeaderData) override {
      if (this->refreshAttempted) {
        return;
      }
      this->refreshAttempted = true;

      try {
        this->releaseHandles();
        this->contextHandleAcquired = false;

        // Round 1: obtain the client's initial token from the credential cache.
        std::string clientToken = this->stepContext("");
        if (clientToken.empty()) {
          throw std::runtime_error(
              "SSPI produced no initial Negotiate token");
        }

        ChallengeResult challenge =
            this->sendWithAuth(curl, responseData, responseHeaderData,
                               "Negotiate " + clientToken);

        if (challenge.httpStatus == 200) {
          // The coordinator accepted the initial token outright.
          this->headers["Authorization"] = "Negotiate " + clientToken;
          this->authenticated            = true;
          this->refreshAttempted         = false;
          WriteLog(LL_INFO, "  Kerberos authentication completed in one round");
          return;
        }

        if (challenge.negotiateChallenge.empty()) {
          throw std::runtime_error(
              "Server returned HTTP " + std::to_string(challenge.httpStatus) +
              " without a Negotiate challenge");
        }

        // Round 2: answer the server's challenge to complete mutual auth.
        std::string finalToken = this->stepContext(challenge.negotiateChallenge);

        std::string authorizationValue =
            finalToken.empty() ? "Negotiate" : "Negotiate " + finalToken;

        ChallengeResult finalResult = this->sendWithAuth(
            curl, responseData, responseHeaderData, authorizationValue);

        if (finalResult.httpStatus != 200) {
          throw std::runtime_error(
              "Kerberos handshake ended with HTTP status " +
              std::to_string(finalResult.httpStatus));
        }

        this->headers["Authorization"] = authorizationValue;
        this->authenticated            = true;
        // Allow a future re-authentication when the ticket ages out.
        this->refreshAttempted = false;
        WriteLog(LL_INFO, "  Kerberos authentication completed");
      } catch (const std::exception& e) {
        /*
        Do not rethrow. refresh() is called from ConnectionConfig::getCurl(),
        which sits underneath ordinary ODBC entry points; an exception escaping
        here would cross the C ABI and terminate the host process. Recording the
        failure instead lets the request go out unauthenticated so Trino's 401
        becomes a normal SQL error that the application can display.
        */
        this->authenticated   = false;
        this->handshakeFailed = true;
        WriteLog(LL_ERROR,
                 "  Kerberos authentication failed: " + std::string(e.what()));
      }
    }

    ~KerberosAuthConfig() override {
      this->releaseHandles();
    }
};

std::unique_ptr<AuthConfig>
getKerberosAuthConfigPtr(std::string hostname,
                         unsigned short port,
                         std::string connectionName,
                         std::string username,
                         std::string servicePrincipalName) {
  return std::make_unique<KerberosAuthConfig>(hostname,
                                              port,
                                              connectionName,
                                              username,
                                              servicePrincipalName);
}
