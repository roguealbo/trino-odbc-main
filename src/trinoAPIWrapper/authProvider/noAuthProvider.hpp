#pragma once

#include <memory>
#include <string>

#include "authConfig.hpp"
#include "authCredentials.hpp"

std::unique_ptr<AuthConfig> getNoAuthConfigPtr(std::string hostname,
                                               unsigned short port,
                                               std::string connectionName,
                                               const AuthCredentials& creds);
