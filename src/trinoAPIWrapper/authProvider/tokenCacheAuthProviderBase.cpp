#include "tokenCacheAuthProviderBase.hpp"

TokenCacheAuthProviderBase::TokenCacheAuthProviderBase(
    std::string hostname, unsigned short port, std::string connectionName)
    : AuthConfig(hostname, port, connectionName) {
  this->tokenId = getTokenIdentity(hostname, port, connectionName);
  this->tokenCache =
      std::optional<TokenCacheEntry>(readTokenCache(this->tokenId));
  if (not this->tokenCache->isExpired()) {
    this->applyToken();
  }
}

bool const TokenCacheAuthProviderBase::isExpired() {
  return this->tokenCache->isExpired();
}

void TokenCacheAuthProviderBase::applyToken() {
  std::string accessToken = this->tokenCache->getAccessToken();
  std::string authKey     = "Authorization";
  std::string authValue   = "Bearer " + accessToken;
  this->headers[authKey] = authValue;
  this->applyUserHeader();
}

/*
Trino's client protocol requires X-Trino-User on every request, and the server
rejects a request whose X-Trino-User does not match the principal in the
presented token. The OIDC providers previously never set this header, so derive
it from the access token's claims the same way the server does.

A provider that already set an explicit user (for example from a DSN username)
keeps that value.
*/
void TokenCacheAuthProviderBase::applyUserHeader() {
  if (this->trinoUser.empty()) {
    json parsed                  = this->tokenCache->getParsedAccessToken();
    std::string user             = parsed.value<std::string>(
        "preferred_username",
        parsed.value<std::string>("sub",
                                  parsed.value<std::string>("email", "")));
    this->trinoUser              = user;
  }
  if (not this->trinoUser.empty()) {
    this->headers["X-Trino-User"] = this->trinoUser;
  }
}

void TokenCacheAuthProviderBase::refresh(
    CURL* curl,
    std::string* responseData,
    std::map<std::string, std::string>* responseHeaderData) {
  applyTokenIfNotExpired();

  // Actually obtain the access token. This function is pure virtual,
  // so subclasses must implement it.
  std::string accessToken =
      this->obtainAccessToken(curl, responseData, responseHeaderData);

  // Cache the token
  this->cacheToken(accessToken);

  // Set the header for all requests
  this->setAccessTokenHeader(accessToken);
}

void TokenCacheAuthProviderBase::applyTokenIfNotExpired() {
  // The first thing to do when asked to refresh the token is to check
  // the token cache to see if we just need to apply it. Not having
  // a token in the request headers yet is an example of an "Expired
  // Token" that requires a "refresh" too.
  if (not this->tokenCache->isExpired()) {
    this->applyToken();
  }
}

void TokenCacheAuthProviderBase::cacheToken(std::string accessToken) {
  // Cache the auth token for later
  this->tokenCache->setAccessToken(accessToken);
  writeTokenCache(this->tokenCache.value());
}

void TokenCacheAuthProviderBase::setAccessTokenHeader(std::string accessToken) {
  // Set the access token into the headers
  std::string authKey   = "Authorization";
  std::string authValue = "Bearer " + accessToken;
  this->headers[authKey] = authValue;

  /*
  A freshly obtained token may carry different claims than a cached one, so
  re-derive X-Trino-User here too. Using operator[] rather than insert() is
  deliberate: insert() silently does nothing when the key already exists, which
  would leave a stale bearer token in place after a refresh.
  */
  this->applyUserHeader();
}
