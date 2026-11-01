#include "TrinoOdbcErrorHandler.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include <nlohmann/json.hpp>
using nlohmann::json;

#include <windows.h>

namespace {

// Thread-safe catalog and state.
std::unordered_map<std::string, TrinoOdbcErrorHandler::Entry>
    g_catalog; // key: trino_name
std::mutex g_mutex;
std::atomic<bool> g_initialized{false};

// Config discovery / reload tracking
std::string g_config_dir;     // optional override via SetConfigDirectory
std::string g_effective_path; // last successfully loaded path
std::filesystem::file_time_type g_last_mtime{};
bool g_loaded_once = false;

// Return directory of this loaded module (driver DLL/SO).
std::string module_dir() {
#if defined(_WIN32)
  HMODULE hm = nullptr;
  if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&module_dir),
                           &hm)) {
    char path[MAX_PATH] = {0};
    DWORD len           = ::GetModuleFileNameA(hm, path, MAX_PATH);
    if (len > 0) {
      std::filesystem::path p(path);
      return p.parent_path().string();
    }
  }
  return std::string();
#else
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void*>(&module_dir), &info) &&
      info.dli_fname) {
    std::filesystem::path p(info.dli_fname);
    return p.parent_path().string();
  }
  return std::string();
#endif
}

// Resolve the default JSON file path to attempt loading.
std::string resolve_default_json_path() {
  if (const char* env = std::getenv("TRINO_ODBC_ERRORMAP_PATH")) {
    return std::string(env);
  }
  if (!g_config_dir.empty()) {
    std::filesystem::path p(g_config_dir);
    p /= "trino_odbc_errors.json";
    return p.string();
  }
  std::string dir = module_dir();
  if (!dir.empty()) {
    std::filesystem::path p(dir);
    p /= "trino_odbc_errors.json";
    return p.string();
  }
  return std::string("trino_odbc_errors.json");
}

} // namespace

// ---------------- Helpers ----------------

bool TrinoOdbcErrorHandler::equalsIgnoreCase(std::string a, std::string b) {
  auto tolow = [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  };
  std::transform(a.begin(), a.end(), a.begin(), tolow);
  std::transform(b.begin(), b.end(), b.begin(), tolow);
  return a == b;
}

bool TrinoOdbcErrorHandler::isWarning(const std::string& sqlstate) {
  return sqlstate.size() >= 2 && sqlstate[0] == '0' &&
         sqlstate[1] == '1'; // 01xxx
}

std::string TrinoOdbcErrorHandler::humanize_name(const std::string& name) {
  std::string out;
  out.reserve(name.size() + 8);
  bool newWord = true;
  for (char c : name) {
    if (c == '_') {
      out.push_back(' ');
      newWord = true;
    } else {
      out.push_back(
          newWord
              ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
              : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      newWord = false;
    }
  }
  return out;
}

// Fallback mapping by Trino errorType (and a couple generic names)
// when we do not have a specific mapping row.
std::string
TrinoOdbcErrorHandler::fallbackByType(const std::string& trinoName,
                                      const std::string& trinoType) {
  if (trinoName == "ALREADY_EXISTS") {
    return "42S01";
  }
  if (trinoName == "NOT_FOUND") {
    return "42S02";
  }

  if (equalsIgnoreCase(trinoType, "USER_ERROR")) {
    return "42000";
  }
  if (equalsIgnoreCase(trinoType, "INSUFFICIENT_RESOURCES")) {
    return "HY001";
  }
  if (equalsIgnoreCase(trinoType, "EXTERNAL")) {
    return "HY000";
  }
  // INTERNAL_ERROR or unknown:
  return "HY000";
}

// ---------------- Catalog build / reload ----------------

void TrinoOdbcErrorHandler::buildCompiledDefaults() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_catalog.clear();

  // NOTE:
  // - Numeric codes taken from Trino StandardErrorCode.java.
  // - sqlstate left empty when there is no exact ODBC equivalent; the code will
  // fallback by type.
  // - description kept short; if empty, we auto-derive from the enum name.

  static const Entry kDefaults[] = {
      // USER_ERROR 0..133
      {0, "GENERIC_USER_ERROR", "42000", "Generic user error"},
      {1, "SYNTAX_ERROR", "42000", "SQL syntax error"},
      {2, "ABANDONED_QUERY", "HY008", "Operation canceled"},
      {3, "USER_CANCELED", "HY008", "Operation canceled by user"},
      {4, "PERMISSION_DENIED", "42000", "Permission denied"},
      {5, "NOT_FOUND", "42S02", "Object not found"},
      {6, "FUNCTION_NOT_FOUND", "42000", "Function not found"},
      {7, "INVALID_FUNCTION_ARGUMENT", "22023", "Invalid function argument"},
      {8, "DIVISION_BY_ZERO", "22012", "Division by zero"},
      {9, "INVALID_CAST_ARGUMENT", "22018", "Invalid cast argument"},
      {10, "OPERATOR_NOT_FOUND", "42000", "Operator not found"},
      {11, "INVALID_VIEW", "42000", "Invalid view"},
      {12, "ALREADY_EXISTS", "42S01", "Object already exists"},
      {13, "NOT_SUPPORTED", "0A000", "Feature not supported"},
      {14, "INVALID_SESSION_PROPERTY", "42000", "Invalid session property"},
      {15, "INVALID_WINDOW_FRAME", "42000", "Invalid window frame"},
      {16, "CONSTRAINT_VIOLATION", "23000", "Integrity constraint violation"},
      {17, "TRANSACTION_CONFLICT", "40001", "Serialization failure"},
      {18, "INVALID_TABLE_PROPERTY", "42000", "Invalid table property"},
      {19, "NUMERIC_VALUE_OUT_OF_RANGE", "22003", "Numeric value out of range"},
      {20, "UNKNOWN_TRANSACTION", "25000", "Unknown transaction"},
      {21, "NOT_IN_TRANSACTION", "25000", "Not in a transaction"},
      {22,
       "TRANSACTION_ALREADY_ABORTED",
       "25S03",
       "Transaction already aborted"},
      {23, "READ_ONLY_VIOLATION", "25000", "Write in read-only context"},
      {24,
       "MULTI_CATALOG_WRITE_CONFLICT",
       "25000",
       "Write conflict across catalogs"},
      {25,
       "AUTOCOMMIT_WRITE_CONFLICT",
       "25000",
       "Write conflict in autocommit"},
      {26,
       "UNSUPPORTED_ISOLATION_LEVEL",
       "0A000",
       "Isolation level not supported"},
      {27, "INCOMPATIBLE_CLIENT", "08001", "Client/server incompatible"},
      {28,
       "SUBQUERY_MULTIPLE_ROWS",
       "21000",
       "Scalar subquery returns multiple rows"},
      {29, "PROCEDURE_NOT_FOUND", "42000", "Stored procedure not found"},
      {30, "INVALID_PROCEDURE_ARGUMENT", "22023", "Invalid procedure argument"},
      {31, "QUERY_REJECTED", "HY000", "Query rejected"},
      {32, "AMBIGUOUS_FUNCTION_CALL", "42000", "Ambiguous function call"},
      {33, "INVALID_SCHEMA_PROPERTY", "42000", "Invalid schema property"},
      {34, "SCHEMA_NOT_EMPTY", "42000", "Schema not empty"},
      {35, "QUERY_TEXT_TOO_LARGE", "HY000", "Query text too large"},
      {36, "UNSUPPORTED_SUBQUERY", "0A000", "Unsupported subquery"},
      {37,
       "EXCEEDED_FUNCTION_MEMORY_LIMIT",
       "HY001",
       "Function exceeded memory limit"},
      {38, "ADMINISTRATIVELY_KILLED", "HY008", "Killed by administrator"},
      {39, "INVALID_COLUMN_PROPERTY", "42000", "Invalid column property"},
      {40, "QUERY_HAS_TOO_MANY_STAGES", "HY000", "Query has too many stages"},
      {41,
       "INVALID_SPATIAL_PARTITIONING",
       "42000",
       "Invalid spatial partitioning"},
      {42, "INVALID_ANALYZE_PROPERTY", "42000", "Invalid analyze property"},
      {43, "TYPE_NOT_FOUND", "07006", "Type not found"},
      {44, "CATALOG_NOT_FOUND", "3D000", "Catalog not found"},
      {45, "SCHEMA_NOT_FOUND", "3F000", "Schema not found"},
      {46, "TABLE_NOT_FOUND", "42S02", "Base table or view not found"},
      {47, "COLUMN_NOT_FOUND", "42S22", "Column not found"},
      {48, "ROLE_NOT_FOUND", "28000", "Role not found"},
      {49, "SCHEMA_ALREADY_EXISTS", "42000", "Schema already exists"},
      {50, "TABLE_ALREADY_EXISTS", "42S01", "Table already exists"},
      {51, "COLUMN_ALREADY_EXISTS", "42S21", "Column already exists"},
      {52, "ROLE_ALREADY_EXISTS", "28000", "Role already exists"},
      {53, "DUPLICATE_NAMED_QUERY", "42000", "Duplicate named query"},
      {54, "DUPLICATE_COLUMN_NAME", "42S21", "Duplicate column name"},
      {55, "MISSING_COLUMN_NAME", "42000", "Missing column name"},
      {56, "MISSING_CATALOG_NAME", "3D000", "Missing catalog name"},
      {57, "MISSING_SCHEMA_NAME", "3F000", "Missing schema name"},
      {58, "TYPE_MISMATCH", "07006", "Type mismatch"},
      {59, "INVALID_LITERAL", "22018", "Invalid literal"},
      {60, "COLUMN_TYPE_UNKNOWN", "07006", "Column type unknown"},
      {61, "MISMATCHED_COLUMN_ALIASES", "42000", "Mismatched column aliases"},
      {62, "AMBIGUOUS_NAME", "42000", "Ambiguous name"},
      {63, "INVALID_COLUMN_REFERENCE", "42000", "Invalid column reference"},
      {64, "MISSING_GROUP_BY", "42000", "Missing GROUP BY"},
      {65, "MISSING_ORDER_BY", "42000", "Missing ORDER BY"},
      {66, "MISSING_OVER", "42000", "Missing OVER clause"},
      {67, "NESTED_AGGREGATION", "42000", "Nested aggregation not allowed"},
      {68, "NESTED_WINDOW", "42000", "Nested window not allowed"},
      {69, "EXPRESSION_NOT_IN_DISTINCT", "42000", "Expression not in DISTINCT"},
      {70, "TOO_MANY_GROUPING_SETS", "42000", "Too many grouping sets"},
      {71, "FUNCTION_NOT_WINDOW", "42000", "Function is not a window function"},
      {72, "FUNCTION_NOT_AGGREGATE", "42000", "Function is not an aggregate"},
      {73, "EXPRESSION_NOT_AGGREGATE", "42000", "Expression not aggregate"},
      {74, "EXPRESSION_NOT_SCALAR", "42000", "Expression not scalar"},
      {75, "EXPRESSION_NOT_CONSTANT", "42000", "Expression not constant"},
      {76, "INVALID_ARGUMENTS", "22023", "Invalid arguments"},
      {77, "TOO_MANY_ARGUMENTS", "07001", "Too many arguments"},
      {78, "INVALID_PRIVILEGE", "28000", "Invalid privilege"},
      {79, "DUPLICATE_PROPERTY", "42000", "Duplicate property"},
      {80, "INVALID_PARAMETER_USAGE", "07006", "Invalid parameter usage"},
      {81, "VIEW_IS_STALE", "42000", "View is stale"},
      {82, "VIEW_IS_RECURSIVE", "42000", "View is recursive"},
      {83, "NULL_TREATMENT_NOT_ALLOWED", "42000", "Null treatment not allowed"},
      {84, "INVALID_ROW_FILTER", "42000", "Invalid row filter"},
      {85, "INVALID_COLUMN_MASK", "42000", "Invalid column mask"},
      {86, "MISSING_TABLE", "42S02", "Missing table"},
      {87,
       "INVALID_RECURSIVE_REFERENCE",
       "42000",
       "Invalid recursive reference"},
      {88, "MISSING_COLUMN_ALIASES", "42000", "Missing column aliases"},
      {89, "NESTED_RECURSIVE", "42000", "Nested recursive construct"},
      {90, "INVALID_LIMIT_CLAUSE", "42000", "Invalid LIMIT clause"},
      {91, "INVALID_ORDER_BY", "42000", "Invalid ORDER BY"},
      {92, "DUPLICATE_WINDOW_NAME", "42000", "Duplicate window name"},
      {93, "INVALID_WINDOW_REFERENCE", "42000", "Invalid window reference"},
      {94, "INVALID_PARTITION_BY", "42000", "Invalid PARTITION BY"},
      {95,
       "INVALID_MATERIALIZED_VIEW_PROPERTY",
       "42000",
       "Invalid materialized view property"},
      {96, "INVALID_LABEL", "42000", "Invalid label"},
      {97, "INVALID_PROCESSING_MODE", "42000", "Invalid processing mode"},
      {98, "INVALID_NAVIGATION_NESTING", "42000", "Invalid navigation nesting"},
      {99, "INVALID_ROW_PATTERN", "42000", "Invalid row pattern"},
      {100,
       "NESTED_ROW_PATTERN_RECOGNITION",
       "42000",
       "Nested row pattern recognition"},
      {101, "TABLE_HAS_NO_COLUMNS", "42000", "Table has no columns"},
      {102, "INVALID_RANGE", "22000", "Invalid range"},
      {103,
       "INVALID_PATTERN_RECOGNITION_FUNCTION",
       "42000",
       "Invalid pattern recognition function"},
      {104, "TABLE_REDIRECTION_ERROR", "HY000", "Table redirection error"},
      {105,
       "MISSING_VARIABLE_DEFINITIONS",
       "42000",
       "Missing variable definitions"},
      {106, "MISSING_ROW_PATTERN", "42000", "Missing row pattern"},
      {107, "INVALID_WINDOW_MEASURE", "42000", "Invalid window measure"},
      {108, "STACK_OVERFLOW", "HY000", "Stack overflow"},
      {109, "MISSING_RETURN_TYPE", "42000", "Missing return type"},
      {110, "AMBIGUOUS_RETURN_TYPE", "42000", "Ambiguous return type"},
      {111, "MISSING_ARGUMENT", "07001", "Missing argument"},
      {112, "DUPLICATE_PARAMETER_NAME", "42000", "Duplicate parameter name"},
      {113, "INVALID_PATH", "22018", "Invalid JSON/path expression"},
      {114,
       "JSON_INPUT_CONVERSION_ERROR",
       "22018",
       "JSON input conversion error"},
      {115,
       "JSON_OUTPUT_CONVERSION_ERROR",
       "22018",
       "JSON output conversion error"},
      {116, "PATH_EVALUATION_ERROR", "22018", "Path evaluation error"},
      {117, "INVALID_JSON_LITERAL", "22018", "Invalid JSON literal"},
      {118, "JSON_VALUE_RESULT_ERROR", "22018", "JSON value result error"},
      {119,
       "MERGE_TARGET_ROW_MULTIPLE_MATCHES",
       "21000",
       "Merge target row multiple matches"},
      {120, "INVALID_COPARTITIONING", "42000", "Invalid copartitioning"},
      {121,
       "INVALID_TABLE_FUNCTION_INVOCATION",
       "42000",
       "Invalid table function invocation"},
      {122, "DUPLICATE_RANGE_VARIABLE", "42000", "Duplicate range variable"},
      {123, "INVALID_CHECK_CONSTRAINT", "23000", "Invalid check constraint"},
      {124, "INVALID_CATALOG_PROPERTY", "42000", "Invalid catalog property"},
      {125, "CATALOG_UNAVAILABLE", "08S01", "Catalog unavailable"},
      {126, "MISSING_RETURN", "42000", "Missing return"},
      {127,
       "DUPLICATE_COLUMN_OR_PATH_NAME",
       "42S21",
       "Duplicate column or path name"},
      {128, "MISSING_PATH_NAME", "42000", "Missing path name"},
      {129, "INVALID_PLAN", "HY000", "Invalid plan"},
      {130, "INVALID_VIEW_PROPERTY", "42000", "Invalid view property"},
      {131, "INVALID_ENTITY_KIND", "42000", "Invalid entity kind"},
      {132,
       "QUERY_EXCEEDED_COMPILER_LIMIT",
       "HY000",
       "Query exceeded compiler limit"},
      {133, "INVALID_FUNCTION_PROPERTY", "42000", "Invalid function property"},

      // INTERNAL_ERROR 65536..65566
      {65536, "GENERIC_INTERNAL_ERROR", "HY000", "Generic internal error"},
      {65537, "TOO_MANY_REQUESTS_FAILED", "08S01", "Too many requests failed"},
      {65538, "PAGE_TOO_LARGE", "HY000", "Page too large"},
      {65539, "PAGE_TRANSPORT_ERROR", "08S01", "Page transport error"},
      {65540, "PAGE_TRANSPORT_TIMEOUT", "08S01", "Page transport timeout"},
      {65541, "NO_NODES_AVAILABLE", "08S01", "No nodes available"},
      {65542, "REMOTE_TASK_ERROR", "08S01", "Remote task error"},
      {65543, "COMPILER_ERROR", "HY000", "Compiler error"},
      {65544, "REMOTE_TASK_MISMATCH", "08S01", "Remote task mismatch"},
      {65545, "SERVER_SHUTTING_DOWN", "08S01", "Server shutting down"},
      {65546,
       "FUNCTION_IMPLEMENTATION_MISSING",
       "0A000",
       "Function implementation missing"},
      {65547,
       "REMOTE_BUFFER_CLOSE_FAILED",
       "08S01",
       "Remote buffer close failed"},
      {65548, "SERVER_STARTING_UP", "08004", "Server starting up"},
      {65549,
       "FUNCTION_IMPLEMENTATION_ERROR",
       "HY000",
       "Function implementation error"},
      {65550,
       "INVALID_PROCEDURE_DEFINITION",
       "42000",
       "Invalid procedure definition"},
      {65551, "PROCEDURE_CALL_FAILED", "HY000", "Procedure call failed"},
      {65552,
       "AMBIGUOUS_FUNCTION_IMPLEMENTATION",
       "42000",
       "Ambiguous function implementation"},
      {65553, "ABANDONED_TASK", "HY008", "Task abandoned"},
      {65554,
       "CORRUPT_SERIALIZED_IDENTITY",
       "HY000",
       "Corrupt serialized identity"},
      {65555, "CORRUPT_PAGE", "HY000", "Corrupt page"},
      {65556, "OPTIMIZER_TIMEOUT", "HYT00", "Optimizer timeout"},
      {65557, "OUT_OF_SPILL_SPACE", "HY001", "Out of spill space"},
      {65558, "REMOTE_HOST_GONE", "08S01", "Remote host gone"},
      {65559, "CONFIGURATION_INVALID", "HY000", "Configuration invalid"},
      {65560,
       "CONFIGURATION_UNAVAILABLE",
       "HY000",
       "Configuration unavailable"},
      {65561, "INVALID_RESOURCE_GROUP", "HY000", "Invalid resource group"},
      {65562, "SERIALIZATION_ERROR", "HY000", "Serialization error"},
      {65563, "REMOTE_TASK_FAILED", "08S01", "Remote task failed"},
      {65564,
       "EXCHANGE_MANAGER_NOT_CONFIGURED",
       "HY000",
       "Exchange manager not configured"},
      {65565, "CATALOG_NOT_AVAILABLE", "08S01", "Catalog not available"},
      {65566, "CATALOG_STORE_ERROR", "HY000", "Catalog store error"},

      // INSUFFICIENT_RESOURCES 131072..131082
      {131072,
       "GENERIC_INSUFFICIENT_RESOURCES",
       "HY001",
       "Insufficient resources"},
      {131073,
       "EXCEEDED_GLOBAL_MEMORY_LIMIT",
       "HY001",
       "Exceeded global memory limit"},
      {131074, "QUERY_QUEUE_FULL", "HYT00", "Query queue full"},
      {131075, "EXCEEDED_TIME_LIMIT", "HYT00", "Exceeded time limit"},
      {131076, "CLUSTER_OUT_OF_MEMORY", "HY001", "Cluster out of memory"},
      {131077, "EXCEEDED_CPU_LIMIT", "HY000", "Exceeded CPU limit"},
      {131078, "EXCEEDED_SPILL_LIMIT", "HY000", "Exceeded spill limit"},
      {131079,
       "EXCEEDED_LOCAL_MEMORY_LIMIT",
       "HY001",
       "Exceeded local memory limit"},
      {131080,
       "ADMINISTRATIVELY_PREEMPTED",
       "HY008",
       "Administratively preempted"},
      {131081, "EXCEEDED_SCAN_LIMIT", "HY000", "Exceeded scan limit"},
      {131082,
       "EXCEEDED_TASK_DESCRIPTOR_STORAGE_CAPACITY",
       "HY001",
       "Exceeded task descriptor storage capacity"},

      // EXTERNAL
      {133001, "UNSUPPORTED_TABLE_TYPE", "0A000", "Unsupported table type"}};

  for (const auto& row : kDefaults) {
    g_catalog[row.trino_name] = row;
  }
}

void TrinoOdbcErrorHandler::ensureInitialized() {
  bool expected = false;
  if (g_initialized.compare_exchange_strong(expected, true)) {
    buildCompiledDefaults();
    // Best-effort initial autoload.
    ReloadMappingFromJson({});
  }
}

void TrinoOdbcErrorHandler::maybeAutoReloadFromDisk() {
  const std::string path = resolve_default_json_path();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return;
  }

  auto mtime = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return;
  }

  if (!g_loaded_once || mtime != g_last_mtime) {
    ReloadMappingFromJson(path);
  }
}

// ---------------- Public API ----------------

const TrinoOdbcErrorHandler::Entry*
TrinoOdbcErrorHandler::LookupEntryByName(const std::string& errorName) {

  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_catalog.find(errorName);
  return (it != g_catalog.end()) ? &it->second : nullptr;
}

bool TrinoOdbcErrorHandler::ReloadMappingFromJson(const std::string& path,
                                                  std::string* error_out) {
  ensureInitialized();

  const std::string file = !path.empty() ? path : resolve_default_json_path();

  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) {
    if (error_out) {
      *error_out = "JSON file not found: " + file;
    }
    return false;
  }

  std::ifstream in(file);
  if (!in) {
    if (error_out) {
      *error_out = "Unable to open JSON file: " + file;
    }
    return false;
  }

  json j = json::parse(in, nullptr, false);
  if (j.is_discarded()) {
    if (error_out) {
      *error_out = "Invalid JSON syntax in: " + file;
    }
    return false;
  }

  if (!j.is_object() || !j.contains("entries") || !j["entries"].is_array()) {
    if (error_out) {
      *error_out = "JSON must contain an 'entries' array: " + file;
    }
    return false;
  }

  std::lock_guard<std::mutex> lock(g_mutex);

  for (const auto& item : j["entries"]) {
    if (!item.is_object()) {
      continue;
    }
    if (!item.contains("name") || !item["name"].is_string()) {
      continue;
    }

    Entry e{};
    e.trino_name = item["name"].get<std::string>();

    if (item.contains("trino_code") && item["trino_code"].is_number_integer()) {
      e.trino_code = item["trino_code"].get<int>();
    } else {
      // If not provided, keep existing or -1.
      auto it      = g_catalog.find(e.trino_name);
      e.trino_code = (it != g_catalog.end()) ? it->second.trino_code : -1;
    }

    if (item.contains("sqlstate") && item["sqlstate"].is_string()) {
      e.sqlstate = item["sqlstate"].get<std::string>();
    } else {
      auto it = g_catalog.find(e.trino_name);
      e.sqlstate =
          (it != g_catalog.end()) ? it->second.sqlstate : std::string();
    }

    if (item.contains("description") && item["description"].is_string()) {
      e.description = item["description"].get<std::string>();
    } else {
      auto it = g_catalog.find(e.trino_name);
      e.description =
          (it != g_catalog.end()) ? it->second.description : std::string();
    }

    g_catalog[e.trino_name] = std::move(e);
  }

  std::error_code ec2;
  g_last_mtime     = std::filesystem::last_write_time(file, ec2);
  g_effective_path = file;
  g_loaded_once    = true;
  return true;
}

void TrinoOdbcErrorHandler::SetConfigDirectory(const std::string& dir) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_config_dir = dir;
}

std::string TrinoOdbcErrorHandler::GetEffectiveConfigPath() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_effective_path;
}

TrinoOdbcErrorHandler::OdbcError
TrinoOdbcErrorHandler::FromTrinoJson(const nlohmann::json& err,
                                     const std::string& queryId) {
  ensureInitialized();
  maybeAutoReloadFromDisk();

  OdbcError out{};

  if (!(err.contains("errorName") && err.contains("errorType") &&
        err.contains("errorCode"))) {
    out.sqlstate    = "HY000";
    out.native      = 0;
    out.ret         = SQL_ERROR;
    out.message     = "[Trino] Unexpected error payload (missing fields).";
    out.description = "Unexpected error payload";
    return out;
  }

  const std::string name = err["errorName"].get<std::string>();
  const std::string type = err["errorType"].get<std::string>();
  const SQLINTEGER code  = static_cast<SQLINTEGER>(err["errorCode"].get<int>());

  // Find mapping row (after overrides)
  const Entry* row = LookupEntryByName(name);

  // Decide SQLSTATE: prefer mapped row; else fallback by type/name.
  std::string sqlstate = (row && !row->sqlstate.empty())
                             ? row->sqlstate
                             : fallbackByType(name, type);

  // Message
  std::ostringstream oss;
  oss << "[Trino] " << type << ": " << name << " (" << code << ")";
  if (!queryId.empty()) {
    oss << " [queryId=" << queryId << "]";
  }
  if (err.contains("message")) {
    oss << " - " << err["message"].get<std::string>();
  }

  out.queryId     = queryId;
  out.sqlstate    = sqlstate;
  out.native      = code; // Trino native code
  out.message     = oss.str();
  out.description = (row && !row->description.empty()) ? row->description
                                                       : humanize_name(name);
  // Parse out the stack
  if (err.contains("failureInfo")) {

    auto failureInfo = err["failureInfo"];

    if (failureInfo.contains("stack")) {
      out.stack.push_back("Stack:");
      for (const auto& frame : failureInfo["stack"]) {
        out.stack.push_back("\t" + frame.get<std::string>());
      }
    }

    // Cause (nested stack)
    if (failureInfo.contains("cause") &&
        failureInfo["cause"].contains("type") &&
        failureInfo["cause"].contains("stack")) {
      out.stack.push_back("Caused By:");
      for (const auto& frame : failureInfo["cause"]["stack"]) {
        out.stack.push_back("\t" + frame.get<std::string>());
      }
    }
  }

  if (err.contains("errorLocation") && err["errorLocation"].is_object()) {
    const auto& loc = err["errorLocation"];
    if (loc.contains("lineNumber") && loc["lineNumber"].is_number_integer()) {
      out.lineNumber = loc["lineNumber"].get<int>();
    }
    if (loc.contains("columnNumber") &&
        loc["columnNumber"].is_number_integer()) {
      out.columnNumber = loc["columnNumber"].get<int>();
    }
  } else if (err.contains("failureInfo") &&
             err["failureInfo"].contains("errorLocation")) {
    const auto& loc = err["failureInfo"]["errorLocation"];
    if (loc.contains("lineNumber") && loc["lineNumber"].is_number_integer()) {
      out.lineNumber = loc["lineNumber"].get<int>();
    }
    if (loc.contains("columnNumber") &&
        loc["columnNumber"].is_number_integer()) {
      out.columnNumber = loc["columnNumber"].get<int>();
    }
  }

  out.ret = isWarning(sqlstate) ? SQL_SUCCESS_WITH_INFO : SQL_ERROR;
  return out;
}

std::string TrinoOdbcErrorHandler::OdbcErrorToString(const OdbcError& err,
                                                     bool include_stack) {
  std::ostringstream oss;
  oss << "Trino Error Information(queryId:" << err.queryId << ")\n";
  oss << "\tret: " << err.ret << "\n";
  oss << "\tsqlstate: " << err.sqlstate << "\n";
  oss << "\tnative: " << err.native << "\n";
  oss << "\tdescription: " << err.description << "\n";
  oss << "\tmessage: " << err.message << "\n";
  if (err.lineNumber) {
    oss << "\tlineNumber:" << err.lineNumber.value() << "\n";
  }

  if (err.columnNumber) {
    oss << "\tcolumnNumber:" << err.columnNumber.value() << "\n";
  }

  if (include_stack) {
    for (const auto& entry : err.stack) {
      oss << "\t" << entry << "\n";
    }
  }

  return oss.str();
}
