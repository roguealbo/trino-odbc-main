#pragma once

#include <string>

std::string userEncryptString(const std::string& text);
std::string userDecryptString(const std::string& text);
std::string systemEncryptString(const std::string& text);
std::string systemDecryptString(const std::string& text);

/*
Standard base64 (not base64url) encoding and decoding.

These are used to build HTTP Basic credentials and to encode/decode the
GSS-API/SSPI tokens used by Negotiate (Kerberos) authentication. Both are
implemented with the Windows CryptBinaryToString/CryptStringToBinary APIs so
the driver does not need an extra crypto dependency.
*/
std::string base64Encode(const std::string& text);
std::string base64Decode(const std::string& text);
