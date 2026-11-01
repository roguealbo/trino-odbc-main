#pragma once

/** TrinoOdbcErrorHandler.h
 *
 * Maps Trino error payloads (errorName, errorType, errorCode, message)
 * to ODBC diagnostics (SQLSTATE, native code) and SQLRETURN.
 *
 * Usage example:
 *   auto diag = TrinoOdbcErrorHandler::FromTrinoJson(trinoErrorJson,
 *   getQueryId()); TrinoOdbcErrorHandler::PostDiag(SQL_HANDLE_STMT, hstmt,
 *   diag); return diag.ret;  // typically SQL_ERROR
 *
 * References :
 * Trino StandardErrorCode(names + types)
 *    see
 * https://github.com/trinodb/trino/blob/master/core/trino-spi/src/main/java/io/trino/spi/StandardErrorCode.java
 * ODBC SQLSTATE Appendix A(Microsoft ODBC)
 *    see
 * https://learn.microsoft.com/en-us/sql/odbc/reference/appendixes/appendix-a-odbc-error-codes?view=sql-server-ver17
 */

#include "../util/windowsLean.hpp"

#include <optional>
#include <sql.h>
#include <sqlext.h>
#include <string>
#include <vector>

// Keep this header light: forward declare nlohmann::json
#include <nlohmann/json_fwd.hpp>

class TrinoOdbcErrorHandler {
  public:
    // Result your ODBC entry points can use.
    struct OdbcError {
        SQLRETURN ret;        // SQL_ERROR or SQL_SUCCESS_WITH_INFO
        std::string sqlstate; // ODBC error code e.g. "42000"
        SQLINTEGER native;    // typically the Trino errorCode
        std::string message;  // user-visible message
        std::string
            description; // short description from the table (or auto-derived)
        std::vector<std::string> stack; // stack trace of the error
        std::optional<int> lineNumber;
        std::optional<int> columnNumber;
        std::string queryId;
    };

    // Single catalog row (exactly your requested shape).
    struct Entry {
        int trino_code;         // e.g. 46 for TABLE_NOT_FOUND
        std::string trino_name; // e.g. "TABLE_NOT_FOUND"
        std::string sqlstate;   // e.g. "42S02"
        std::string
            description; // short phrase; can be empty (auto-derived when used)
    };

    // Build ODBC diagnostics from a Trino error JSON (fields: errorName,
    // errorType, errorCode, message).
    static OdbcError FromTrinoJson(const nlohmann::json& err,
                                   const std::string& queryId);

    // Lookup the current catalog entry by Trino error name (after any JSON
    // overrides). Returns nullptr if not found.
    static const Entry* LookupEntryByName(const std::string& errorName);

    // Reload (or initial load) from JSON. If 'path' is empty, uses the default
    // search order. Returns true if a file was found and loaded successfully.
    // Optional error text in error_out.
    static bool ReloadMappingFromJson(const std::string& path = {},
                                      std::string* error_out  = nullptr);

    // Optionally pin a directory to search for trino_odbc_errors.json (after
    // explicit path, before module dir).
    static void SetConfigDirectory(const std::string& dir);

    // Path of the last successfully loaded JSON file (empty if never loaded).
    static std::string GetEffectiveConfigPath();

    static std::string OdbcErrorToString(const OdbcError& err,
                                         bool include_stack);

  private:
    // Helpers
    static std::string fallbackByType(const std::string& trinoName,
                                      const std::string& trinoType);
    static bool isWarning(const std::string& sqlstate);
    static bool equalsIgnoreCase(std::string a, std::string b);
    static std::string humanize_name(const std::string& name);

    // Catalog management
    static void ensureInitialized();
    static void buildCompiledDefaults(); // fill the map from the compiled table
    static void maybeAutoReloadFromDisk();
};
