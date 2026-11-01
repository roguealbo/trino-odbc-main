#include "../util/windowsLean.hpp"
#include <sql.h>
#include <sqlext.h>

#include "handles/connHandle.hpp"
#include "handles/descriptorHandle.hpp"
#include "handles/envHandle.hpp"
#include "handles/statementHandle.hpp"

#include "../util/valuePtrHelper.hpp"
#include "../util/writeLog.hpp"

SQLRETURN SQL_API SQLGetDiagRec(SQLSMALLINT HandleType,
                                SQLHANDLE Handle,
                                SQLSMALLINT RecNumber,
                                _Out_writes_opt_(6) SQLCHAR* SqlStatePtr,
                                SQLINTEGER* NativeErrorPtr,
                                _Out_writes_opt_(BufferLength)
                                    SQLCHAR* MessageTextPtr,
                                SQLSMALLINT BufferLength,
                                _Out_opt_ SQLSMALLINT* TextLengthPtr) {
  /*
  Return a series of 1-indexed diagnostic records from various handles.
  If a record is requested beyond what is actually available, return
  SQL_NO_DATA instead.
  */
  WriteLog(LL_ERROR,
           "Entering SQLGetDiagRec. HandleType= " + std::to_string(HandleType));
  switch (HandleType) {
    case (SQL_HANDLE_ENV): {
      Environment* env = reinterpret_cast<Environment*>(Handle);
      WriteLog(LL_ERROR, "  Requesting diagnostics for environment handle");
      WriteLog(LL_ERROR,
               "  Requesting RecNumber: " + std::to_string(RecNumber));
      return SQL_NO_DATA;
    }
    case (SQL_HANDLE_DBC): {
      Connection* conn = reinterpret_cast<Connection*>(Handle);
      WriteLog(LL_ERROR, "  Requesting diagnostics for connection handle");
      WriteLog(LL_ERROR,
               "  Requesting RecNumber: " + std::to_string(RecNumber));
      ErrorInfo errorInfo = conn->getError();
      if (RecNumber == 1 and errorInfo.errorOccurred()) {
        writeNullTermStringToPtr<SQLINTEGER>(
            SqlStatePtr, errorInfo.sqlStateCode, nullptr);

        writeNullTermStringToPtr(
            MessageTextPtr, errorInfo.errorMessage, TextLengthPtr);

        if (NativeErrorPtr != nullptr) {
          *NativeErrorPtr =
              -1; // If a valid pointer was provided set the native error code
        }

        return SQL_SUCCESS;
      } else {
        return SQL_NO_DATA;
      }
    }
    case (SQL_HANDLE_STMT): {
      Statement* statement = reinterpret_cast<Statement*>(Handle);
      WriteLog(LL_ERROR, "  Requesting diagnostics for statement handle");
      WriteLog(LL_ERROR,
               "  Requesting RecNumber: " + std::to_string(RecNumber));

      if (statement->trinoQuery->hasError()) {
        TrinoOdbcErrorHandler::OdbcError odbcErr =
            statement->trinoQuery->getError();

        // Only set SqlStatePtr and NativeErrorPtr on the first chunk
        if (RecNumber == 1) {
          writeNullTermStringToPtr<SQLINTEGER>(
              SqlStatePtr, odbcErr.sqlstate, nullptr);

          if (NativeErrorPtr) {
            *NativeErrorPtr = odbcErr.native;
          }
        }

        // Build all lines: summary (split by newlines) + stack (each with tab)
        std::vector<std::string> lines;
        {
          std::istringstream summaryStream(
              TrinoOdbcErrorHandler::OdbcErrorToString(odbcErr, false));
          std::string line;
          while (std::getline(summaryStream, line)) {
            lines.push_back(line);
          }
        }
        for (const auto& entry : odbcErr.stack) {
          lines.push_back("\t" + entry);
        }

        // Now, chunk by lines, not by bytes
        size_t chunkSize =
            BufferLength > 0 ? static_cast<size_t>(BufferLength - 1) : 0;
        size_t currentChunk = 1;
        size_t currentLen   = 0;
        std::string chunk;
        bool found = false;

        for (size_t i = 0; i < lines.size();) {
          size_t tempLen = 1;
          std::string tempChunk;
          tempChunk += "\n";
          size_t j = i;
          for (; j < lines.size(); ++j) {
            // +1 for the newline
            size_t lineLen = lines[j].size() + 1;
            if (tempLen + lineLen > chunkSize && tempLen > 0) {
              break; // Don't add this line, chunk is full
            }
            tempChunk += lines[j] + "\n";
            tempLen += lineLen;
          }
          if (currentChunk == static_cast<size_t>(RecNumber)) {
            chunk = tempChunk;
            found = true;
            break;
          }
          ++currentChunk;
          i = j;
        }

        if (!found || chunk.empty()) {
          return SQL_NO_DATA;
        }

        // Copy the chunk to the output buffer
        size_t toCopy = std::min(chunk.size(), chunkSize);
        if (MessageTextPtr && BufferLength > 0) {
          strncpy_s(reinterpret_cast<char*>(MessageTextPtr),
                    BufferLength,
                    chunk.c_str(),
                    toCopy);
          reinterpret_cast<char*>(MessageTextPtr)[toCopy] = '\0';
        }
        if (TextLengthPtr) {
          *TextLengthPtr = static_cast<SQLSMALLINT>(toCopy);
        }

        return SQL_SUCCESS;
      } else {
        return SQL_NO_DATA;
      }
    }
    case (SQL_HANDLE_DESC): {
      Descriptor* descriptor = reinterpret_cast<Descriptor*>(Handle);
      WriteLog(LL_ERROR, "  Requesting diagnostics for descriptor handle");
      WriteLog(LL_ERROR,
               "  Requesting RecNumber: " + std::to_string(RecNumber));
      return SQL_NO_DATA;
    }
    default: {
      WriteLog(LL_ERROR, "  ERROR: Unknown handle type in SQLGetDiagRec");
      return SQL_ERROR;
    }
  }
}
