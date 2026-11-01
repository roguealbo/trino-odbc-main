#include "dsnConfigForm.hpp"

#include "../../util/stringFromChar.hpp"
#include "../../util/writeLog.hpp"
#include "win32controls/buttonMaker.hpp"
#include "win32controls/comboboxMaker.hpp"
#include "win32controls/editMaker.hpp"
#include "win32controls/labelMaker.hpp"

#include <CommCtrl.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

// Define some identifiers for buttons and fields in the edit form.
constexpr int ID_EDIT_DSN             = 101;
constexpr int ID_EDIT_HOSTNAME        = 102;
constexpr int ID_EDIT_PORT            = 103;
constexpr int ID_BUTTON_SAVE          = 104;
constexpr int ID_BUTTON_CANCEL        = 105;
constexpr int ID_COMBO_LOGLEVEL       = 106;
constexpr int ID_COMBO_AUTHMETHOD     = 107;
constexpr int ID_EDIT_OIDC_DISC       = 108;
constexpr int ID_EDIT_CLIENT_ID       = 109;
constexpr int ID_EDIT_CLIENT_SECRET   = 110;
constexpr int ID_EDIT_OIDC_SCOPE      = 111;
constexpr int ID_STATIC_OIDC_DISC     = 112;
constexpr int ID_STATIC_CLIENT_ID     = 113;
constexpr int ID_STATIC_CLIENT_SECRET = 114;
constexpr int ID_STATIC_OIDC_SCOPE    = 115;
constexpr int ID_EDIT_USERNAME        = 116;
constexpr int ID_STATIC_USERNAME      = 117;
constexpr int ID_EDIT_PASSWORD        = 118;
constexpr int ID_STATIC_PASSWORD      = 119;
constexpr int ID_EDIT_TOKEN           = 120;
constexpr int ID_STATIC_TOKEN         = 121;
constexpr int ID_EDIT_TOKEN_FILE      = 122;
constexpr int ID_STATIC_TOKEN_FILE    = 123;
constexpr int ID_EDIT_TOKEN_COMMAND   = 124;
constexpr int ID_STATIC_TOKEN_COMMAND = 125;
constexpr int ID_EDIT_SPN             = 126;
constexpr int ID_STATIC_SPN           = 127;
constexpr int ID_EDIT_SSL_CA          = 128;
constexpr int ID_STATIC_SSL_CA        = 129;
constexpr int ID_EDIT_SSL_VERIFY      = 130;
constexpr int ID_STATIC_SSL_VERIFY    = 131;
constexpr int ID_EDIT_TIMEOUT         = 132;
constexpr int ID_STATIC_TIMEOUT       = 133;
constexpr int ID_EDIT_CLIENT_INFO     = 134;
constexpr int ID_STATIC_CLIENT_INFO   = 135;
constexpr int BUF_LEN                 = 1024;

std::string NOT_REQUIRED = "<Not Required>";

/*
Layout geometry.

The form grew from a single column to two columns once the non-interactive
authentication options were added, so that every setting stays visible at once
rather than requiring a scroll on the machines where gateway DSNs get created.
*/
constexpr int LEFT_LABEL_X   = 10;
constexpr int LEFT_FIELD_X   = 130;
constexpr int LEFT_FIELD_W   = 165;
constexpr int RIGHT_LABEL_X  = 315;
constexpr int RIGHT_FIELD_X  = 435;
constexpr int RIGHT_FIELD_W  = 165;
constexpr int LABEL_W        = 115;
constexpr int ROW_START      = 10;
constexpr int ROW_SPACING    = 30;
constexpr int BUTTON_ROW     = 410;
constexpr int FORM_WIDTH     = 625;
constexpr int FORM_HEIGHT    = 480;

static int row(int index) {
  return ROW_START + index * ROW_SPACING;
}

/*
Which fields a given authentication method actually uses.

Everything else is greyed out and filled with a placeholder. The form keeps all
fields visible (rather than hiding them) so the layout never jumps around while
the user is filling it in, which matches the approach the original OIDC fields
already took.
*/
static std::vector<int> fieldsForAuthMethod(const std::string& authMethod) {
  if (authMethod == "Basic Auth") {
    return {ID_EDIT_USERNAME, ID_EDIT_PASSWORD};
  }
  if (authMethod == "Token Auth") {
    return {ID_EDIT_USERNAME,
            ID_EDIT_TOKEN,
            ID_EDIT_TOKEN_FILE,
            ID_EDIT_TOKEN_COMMAND};
  }
  if (authMethod == "Kerberos Auth") {
    return {ID_EDIT_USERNAME, ID_EDIT_SPN};
  }
  if (authMethod == "OIDC Client Cred Auth" || authMethod == "Device Flow") {
    return {ID_EDIT_OIDC_DISC,
            ID_EDIT_CLIENT_ID,
            ID_EDIT_CLIENT_SECRET,
            ID_EDIT_OIDC_SCOPE};
  }
  // No Auth and External Auth: the username is still meaningful for No Auth,
  // and harmless (ignored) for External Auth.
  return {ID_EDIT_USERNAME};
}

/*
All the credential fields that can be greyed out, so the toggle logic can
reset any of them, not just the ones belonging to the current method.
*/
static const std::vector<int> ALL_CREDENTIAL_FIELDS = {
    ID_EDIT_USERNAME,      ID_EDIT_PASSWORD,      ID_EDIT_TOKEN,
    ID_EDIT_TOKEN_FILE,    ID_EDIT_TOKEN_COMMAND, ID_EDIT_SPN,
    ID_EDIT_OIDC_DISC,     ID_EDIT_CLIENT_ID,     ID_EDIT_CLIENT_SECRET,
    ID_EDIT_OIDC_SCOPE};

static std::string readField(HWND edit) {
  /*
  Read a field's text, treating the greyed-out placeholder as empty. Without
  this, a field the user never touched would be saved to the DSN containing the
  literal string "<Not Required>".
  */
  std::string text = getEditText(edit);
  if (text == NOT_REQUIRED) {
    return "";
  }
  return text;
}

static void setFieldState(HWND edit, bool relevant) {
  if (edit == nullptr) {
    return;
  }
  if (relevant) {
    if (getEditText(edit) == NOT_REQUIRED) {
      setEditText(edit, "");
    }
    setEditWriteable(edit);
  } else {
    setEditReadOnly(edit);
    if (getEditText(edit) == "") {
      setEditText(edit, NOT_REQUIRED);
    }
  }
}

static void applyAuthMethodState(HWND hwnd, const std::string& authMethod) {
  std::vector<int> relevant = fieldsForAuthMethod(authMethod);
  for (int fieldId : ALL_CREDENTIAL_FIELDS) {
    HWND edit = GetDlgItem(hwnd, fieldId);
    bool isRelevant =
        std::find(relevant.begin(), relevant.end(), fieldId) != relevant.end();
    setFieldState(edit, isRelevant);
  }
  InvalidateRect(hwnd, NULL, FALSE);
}

// Simple constructor that saves a copy of the parent window handle.
DSNForm::DSNForm(HWND parent, std::map<std::string, std::string> attributes) {
  this->parent = parent;

  /*
  The attributes arrive from the ODBC driver manager, so the key casing is not
  guaranteed. Look each one up case-insensitively by building a lowercase index
  once.
  */
  std::map<std::string, std::string> lowered;
  for (const auto& pair : attributes) {
    std::string key = pair.first;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    lowered[key] = pair.second;
  }

  /*
  Fall back to the documented defaults rather than leaving fields blank, so a
  brand-new DSN opens showing port 8080 and log level None instead of an empty
  port that would fail validation. This mirrors what readFromPrivateProfile does
  for a DSN, and reuses the same table so the two cannot drift apart.
  */
  auto lookup = [&lowered](const std::string& key) -> std::string {
    auto it = lowered.find(key);
    if (it != lowered.end() and not it->second.empty()) {
      return it->second;
    }
    auto defaultIt = DRIVER_CONFIG_DEFAULT_VALUES.find(key);
    if (defaultIt != DRIVER_CONFIG_DEFAULT_VALUES.end()) {
      return defaultIt->second;
    }
    return "";
  };

  if (not lookup("dsn").empty()) {
    this->configResult.setDSN(lookup("dsn"));
  }
  this->configResult.setHostname(lookup("hostname"));
  this->configResult.setPort(lookup("port"));
  this->configResult.setLogLevel(lookup("loglevel"));
  {
    /*
    Tolerate an unrecognised value here. These attributes arrive from the ODBC
    driver manager, so they may be anything a user or an application typed, and
    this constructor runs inside ConfigDSN where an exception would unwind
    through the driver manager and kill the host process. The form simply falls
    back to the default method; the strict validation happens at connect time,
    where the failure can be reported as a proper SQL error.
    */
    try {
      this->configResult.setAuthMethod(lookup("authmethod"));
    } catch (const std::exception& e) {
      WriteLog(LL_WARN,
               "  DSN form received an invalid auth method: " +
                   std::string(e.what()));
    }
  }
  if (not lookup("oidcdiscoveryurl").empty()) {
    this->configResult.setOidcDiscoveryUrl(lookup("oidcdiscoveryurl"));
  }
  if (not lookup("clientid").empty()) {
    this->configResult.setClientId(lookup("clientid"));
  }
  if (not lookup("clientsecret").empty()) {
    this->configResult.setClientSecret(lookup("clientsecret"));
  }
  if (not lookup("oidcscope").empty()) {
    this->configResult.setOidcScope(lookup("oidcscope"));
  }
  if (not lookup("username").empty()) {
    this->configResult.setUsername(lookup("username"));
  }
  if (not lookup("password").empty()) {
    this->configResult.setPassword(lookup("password"));
  }
  if (not lookup("token").empty()) {
    this->configResult.setToken(lookup("token"));
  }
  if (not lookup("tokenfile").empty()) {
    this->configResult.setTokenFile(lookup("tokenfile"));
  }
  if (not lookup("tokencommand").empty()) {
    this->configResult.setTokenCommand(lookup("tokencommand"));
  }
  if (not lookup("serviceprincipalname").empty()) {
    this->configResult.setServicePrincipalName(lookup("serviceprincipalname"));
  }
  if (not lookup("sslcainfo").empty()) {
    this->configResult.setSslCAInfo(lookup("sslcainfo"));
  }
  if (not lookup("sslverify").empty()) {
    this->configResult.setSslVerify(lookup("sslverify"));
  }
  if (not lookup("requesttimeoutms").empty()) {
    this->configResult.setRequestTimeoutMs(lookup("requesttimeoutms"));
  }
  if (not lookup("clientinfo").empty()) {
    this->configResult.setClientInfo(lookup("clientinfo"));
  }
}


LRESULT CALLBACK WINDOW_CB(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  // This is the window "procedure" for the DSN Config window, which is
  // essentially a callback function. This is where we interact with the window
  // to extract values from it.
  DriverConfig* driverConfigPtr = nullptr;
  if (uMsg == WM_CREATE) {
    CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
    driverConfigPtr = reinterpret_cast<DriverConfig*>(pCreate->lpCreateParams);
    SetWindowLongPtr(
        hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(driverConfigPtr));
  } else {
    driverConfigPtr =
        reinterpret_cast<DriverConfig*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  }

  switch (uMsg) {
    case WM_COMMAND: {
      WriteLog(LL_TRACE, "  WM_COMMAND executing");
      HWND dsnItem         = GetDlgItem(hwnd, ID_EDIT_DSN);
      HWND hostnameItem    = GetDlgItem(hwnd, ID_EDIT_HOSTNAME);
      HWND portItem        = GetDlgItem(hwnd, ID_EDIT_PORT);
      HWND logLevelItem    = GetDlgItem(hwnd, ID_COMBO_LOGLEVEL);
      HWND authMethodItem  = GetDlgItem(hwnd, ID_COMBO_AUTHMETHOD);
      char buf[BUF_LEN]    = {0};
      switch (LOWORD(wParam)) {
        case ID_BUTTON_SAVE: {
          WriteLog(LL_TRACE, "  ID_BUTTON_SAVE executing");

          // Handle the DSN text
          GetWindowText(dsnItem, buf, BUF_LEN);
          driverConfigPtr->setDSN(std::string(buf));

          // Handle the hostname
          GetWindowText(hostnameItem, buf, BUF_LEN);
          driverConfigPtr->setHostname(std::string(buf));

          // Handle the port
          GetWindowText(portItem, buf, BUF_LEN);
          driverConfigPtr->setPort(std::string(buf));

          // Handle the log level
          LRESULT logIndex = SendMessage(logLevelItem, CB_GETCURSEL, 0, 0);
          SendMessage(logLevelItem, CB_GETLBTEXT, logIndex, (LPARAM)buf);
          driverConfigPtr->setLogLevel(std::string(buf));

          // Handle the auth method
          LRESULT authIndex = SendMessage(authMethodItem, CB_GETCURSEL, 0, 0);
          SendMessage(authMethodItem, CB_GETLBTEXT, authIndex, (LPARAM)buf);
          driverConfigPtr->setAuthMethod(std::string(buf));

          // Handle every text field. Fields that are greyed out read back as
          // the placeholder and are stored as empty strings.
          driverConfigPtr->setUsername(
              readField(GetDlgItem(hwnd, ID_EDIT_USERNAME)));
          driverConfigPtr->setPassword(
              readField(GetDlgItem(hwnd, ID_EDIT_PASSWORD)));
          driverConfigPtr->setToken(
              readField(GetDlgItem(hwnd, ID_EDIT_TOKEN)));
          driverConfigPtr->setTokenFile(
              readField(GetDlgItem(hwnd, ID_EDIT_TOKEN_FILE)));
          driverConfigPtr->setTokenCommand(
              readField(GetDlgItem(hwnd, ID_EDIT_TOKEN_COMMAND)));
          driverConfigPtr->setServicePrincipalName(
              readField(GetDlgItem(hwnd, ID_EDIT_SPN)));
          driverConfigPtr->setOidcDiscoveryUrl(
              readField(GetDlgItem(hwnd, ID_EDIT_OIDC_DISC)));
          driverConfigPtr->setClientId(
              readField(GetDlgItem(hwnd, ID_EDIT_CLIENT_ID)));
          driverConfigPtr->setClientSecret(
              readField(GetDlgItem(hwnd, ID_EDIT_CLIENT_SECRET)));
          driverConfigPtr->setOidcScope(
              readField(GetDlgItem(hwnd, ID_EDIT_OIDC_SCOPE)));

          // Transport options. These are always editable regardless of the
          // auth method, so they use the same placeholder-tolerant read.
          driverConfigPtr->setSslCAInfo(
              readField(GetDlgItem(hwnd, ID_EDIT_SSL_CA)));
          driverConfigPtr->setSslVerify(
              readField(GetDlgItem(hwnd, ID_EDIT_SSL_VERIFY)));
          driverConfigPtr->setRequestTimeoutMs(
              readField(GetDlgItem(hwnd, ID_EDIT_TIMEOUT)));
          driverConfigPtr->setClientInfo(
              readField(GetDlgItem(hwnd, ID_EDIT_CLIENT_INFO)));

          driverConfigPtr->setIsSaved(true);

          DestroyWindow(hwnd);
          break;
        }
        case ID_BUTTON_CANCEL: {
          WriteLog(LL_TRACE, "  ID_BUTTON_CANCEL executing");
          driverConfigPtr->setIsSaved(false);

          DestroyWindow(hwnd);
          break;
        }
        default: {
          WriteLog(LL_TRACE, "  WINDOW_CB default (redraw)");
          LRESULT authIndex = SendMessage(authMethodItem, CB_GETCURSEL, 0, 0);
          SendMessage(authMethodItem, CB_GETLBTEXT, authIndex, (LPARAM)buf);
          std::string authMethod = stringFromChar(buf, CHAR_IS_NTS);
          if (not authMethod.empty()) {
            applyAuthMethodState(hwnd, authMethod);
          }
          break;
        }
      }
      break;
    }
    case WM_DESTROY: {
      WriteLog(LL_TRACE, "  WM_DESTROY executing");
      PostQuitMessage(0);
      break;
    }
    default: {
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
  }
  return 0;
}

void DSNForm::ShowDSNForm() {
  WriteLog(LL_DEBUG, "  Creating DSN Form");
  const char className[] = "FormWindowClass";

  WNDCLASS wc      = {};
  wc.lpfnWndProc   = WINDOW_CB;
  wc.hInstance     = GetModuleHandle(NULL);
  wc.lpszClassName = className;
  // Getting data back from a window callback is tricky. The easiest way
  // to do it is to share a pointer.
  wc.cbWndExtra = sizeof(DriverConfig*);

  WriteLog(LL_TRACE, "  Registering class");
  RegisterClass(&wc);

  WriteLog(LL_TRACE, "  Creating window");
  HWND form    = CreateWindowEx(WS_EX_CLIENTEDGE,
                             className,
                             "Enter DSN Information",
                             WS_OVERLAPPED | WS_VISIBLE,
                             CW_USEDEFAULT,
                             CW_USEDEFAULT,
                             FORM_WIDTH,
                             FORM_HEIGHT,
                             this->parent,
                             NULL,
                             GetModuleHandle(NULL),
                             &this->configResult);
  bool visible = true;

  WriteLog(LL_TRACE, "  Creating Labels");
  /*
  Left column: connection identity and the auth method's own fields.

  The label width is passed explicitly. labelMaker's default width (145) would
  run past LEFT_FIELD_X and overlap the text entries, because the two-column
  layout narrowed the gutter that the original single-column form had.
  */
  labelMaker(form, row(0), 0, "DSN Name:", visible, LEFT_LABEL_X, LABEL_W);
  labelMaker(form, row(1), 0, "Hostname:", visible, LEFT_LABEL_X, LABEL_W);
  labelMaker(form, row(2), 0, "Port:", visible, LEFT_LABEL_X, LABEL_W);
  labelMaker(form, row(3), 0, "Log Level:", visible, LEFT_LABEL_X, LABEL_W);
  labelMaker(form, row(4), 0, "Auth Method:", visible, LEFT_LABEL_X, LABEL_W);
  labelMaker(
      form, row(5), ID_STATIC_USERNAME, "Username:", visible, LEFT_LABEL_X, LABEL_W);
  labelMaker(
      form, row(6), ID_STATIC_PASSWORD, "Password:", visible, LEFT_LABEL_X, LABEL_W);
  labelMaker(
      form, row(7), ID_STATIC_CLIENT_ID, "Client ID:", visible, LEFT_LABEL_X, LABEL_W);
  labelMaker(form,
             row(8),
             ID_STATIC_CLIENT_SECRET,
             "Client Secret:",
             visible,
             LEFT_LABEL_X,
             LABEL_W);
  labelMaker(form,
             row(9),
             ID_STATIC_OIDC_DISC,
             "OIDC Discovery URL:",
             visible,
             LEFT_LABEL_X,
             LABEL_W);
  labelMaker(form,
             row(10),
             ID_STATIC_OIDC_SCOPE,
             "OIDC Scope:",
             visible,
             LEFT_LABEL_X,
             LABEL_W);
  labelMaker(
      form, row(11), ID_STATIC_TOKEN, "JWT Token:", visible, LEFT_LABEL_X, LABEL_W);

  // Right column: the remaining non-interactive credential sources plus the
  // transport options that a gateway-hosted DSN needs.
  labelMaker(form,
             row(0),
             0,
             "Advanced / Transport",
             visible,
             RIGHT_LABEL_X,
             LABEL_W);
  labelMaker(form,
             row(1),
             ID_STATIC_TOKEN_FILE,
             "Token File:",
             visible,
             RIGHT_LABEL_X,
             LABEL_W);
  labelMaker(form,
             row(2),
             ID_STATIC_TOKEN_COMMAND,
             "Token Command:",
             visible,
             RIGHT_LABEL_X,
             LABEL_W);
  labelMaker(form,
             row(3),
             ID_STATIC_SPN,
             "Service Principal:",
             visible,
             RIGHT_LABEL_X,
             LABEL_W);
  labelMaker(form,
             row(4),
             ID_STATIC_SSL_CA,
             "CA Bundle Path:",
             visible,
             RIGHT_LABEL_X,
             LABEL_W);
  labelMaker(form,
             row(5),
             ID_STATIC_SSL_VERIFY,
             "Verify TLS:",
             visible,
             RIGHT_LABEL_X,
             LABEL_W);
  labelMaker(form,
             row(6),
             ID_STATIC_TIMEOUT,
             "Timeout (ms):",
             visible,
             RIGHT_LABEL_X,
             LABEL_W);
  labelMaker(form,
             row(7),
             ID_STATIC_CLIENT_INFO,
             "Client Info:",
             visible,
             RIGHT_LABEL_X,
             LABEL_W);

  WriteLog(LL_TRACE, "  Creating Text Entries");
  HWND hwndDsn = editMaker(form, row(0), ID_EDIT_DSN, visible, LEFT_FIELD_X, LEFT_FIELD_W);
  HWND hwndHostname =
      editMaker(form, row(1), ID_EDIT_HOSTNAME, visible, LEFT_FIELD_X, LEFT_FIELD_W);
  HWND hwndPort =
      editMaker(form, row(2), ID_EDIT_PORT, visible, LEFT_FIELD_X, LEFT_FIELD_W);
  HWND hwndUsername =
      editMaker(form, row(5), ID_EDIT_USERNAME, visible, LEFT_FIELD_X, LEFT_FIELD_W);
  // The password and client secret mask what is typed; the JWT is a long
  // credential too and is masked for the same reason.
  HWND hwndPassword = passwordMaker(
      form, row(6), ID_EDIT_PASSWORD, visible, LEFT_FIELD_X, LEFT_FIELD_W);
  HWND hwndClientId =
      editMaker(form, row(7), ID_EDIT_CLIENT_ID, visible, LEFT_FIELD_X, LEFT_FIELD_W);
  HWND hwndClientSecret = passwordMaker(
      form, row(8), ID_EDIT_CLIENT_SECRET, visible, LEFT_FIELD_X, LEFT_FIELD_W);
  HWND hwndOidcDiscoveryUrl = editMaker(
      form, row(9), ID_EDIT_OIDC_DISC, visible, LEFT_FIELD_X, LEFT_FIELD_W);
  HWND hwndOidcScope =
      editMaker(form, row(10), ID_EDIT_OIDC_SCOPE, visible, LEFT_FIELD_X, LEFT_FIELD_W);
  HWND hwndToken = passwordMaker(
      form, row(11), ID_EDIT_TOKEN, visible, LEFT_FIELD_X, LEFT_FIELD_W);

  HWND hwndTokenFile = editMaker(
      form, row(1), ID_EDIT_TOKEN_FILE, visible, RIGHT_FIELD_X, RIGHT_FIELD_W);
  HWND hwndTokenCommand = editMaker(
      form, row(2), ID_EDIT_TOKEN_COMMAND, visible, RIGHT_FIELD_X, RIGHT_FIELD_W);
  HWND hwndSpn      = editMaker(form, row(3), ID_EDIT_SPN, visible, RIGHT_FIELD_X, RIGHT_FIELD_W);
  HWND hwndSslCa    = editMaker(
      form, row(4), ID_EDIT_SSL_CA, visible, RIGHT_FIELD_X, RIGHT_FIELD_W);
  HWND hwndSslVerify = editMaker(
      form, row(5), ID_EDIT_SSL_VERIFY, visible, RIGHT_FIELD_X, RIGHT_FIELD_W);
  HWND hwndTimeout = editMaker(
      form, row(6), ID_EDIT_TIMEOUT, visible, RIGHT_FIELD_X, RIGHT_FIELD_W);
  HWND hwndClientInfo = editMaker(
      form, row(7), ID_EDIT_CLIENT_INFO, visible, RIGHT_FIELD_X, RIGHT_FIELD_W);

  // Sometimes the DSN should be read-only such as when an existing DSN is being
  // configured. In those cases, we need to set the EDIT control to readonly as
  // well.
  if (this->readOnlyDSN) {
    setEditReadOnly(hwndDsn);
  }

  WriteLog(LL_TRACE, "  Creating Log Level Combobox");
  HWND hwndLogLevel =
      comboboxMaker(form, row(3), ID_COMBO_LOGLEVEL, LOG_LEVEL_NAMES, LEFT_FIELD_X, LEFT_FIELD_W);

  WriteLog(LL_TRACE, "  Creating Auth Method Combobox");
  HWND hwndAuthMethod = comboboxMaker(
      form, row(4), ID_COMBO_AUTHMETHOD, AUTH_METHOD_NAMES, LEFT_FIELD_X, LEFT_FIELD_W);

  WriteLog(LL_TRACE, "  Pre-populating the text fields");
  setEditText(hwndDsn, this->configResult.getDSN());
  setEditText(hwndHostname, this->configResult.getHostname());
  setEditText(hwndPort, this->configResult.getPortStr());
  setEditText(hwndUsername, this->configResult.getUsername());
  setEditText(hwndPassword, this->configResult.getPassword());
  setEditText(hwndClientId, this->configResult.getClientId());
  setEditText(hwndClientSecret, this->configResult.getClientSecret());
  setEditText(hwndOidcDiscoveryUrl, this->configResult.getOidcDiscoveryUrl());
  setEditText(hwndOidcScope, this->configResult.getOidcScope());
  setEditText(hwndToken, this->configResult.getToken());
  setEditText(hwndTokenFile, this->configResult.getTokenFile());
  setEditText(hwndTokenCommand, this->configResult.getTokenCommand());
  setEditText(hwndSpn, this->configResult.getServicePrincipalName());
  setEditText(hwndSslCa, this->configResult.getSslCAInfo());
  setEditText(hwndSslVerify,
              this->configResult.getSslVerify() ? "true" : "false");
  setEditText(hwndTimeout, std::to_string(this->configResult.getRequestTimeoutMs()));
  setEditText(hwndClientInfo, this->configResult.getClientInfo());

  WriteLog(LL_TRACE, "  Pre-poplating Comboboxes");
  setCombobox(hwndLogLevel, this->configResult.getLogLevelStr());
  setCombobox(hwndAuthMethod, this->configResult.getAuthMethodStr());

  WriteLog(LL_TRACE, "  Applying auth method field visibility");
  applyAuthMethodState(form, this->configResult.getAuthMethodStr());

  WriteLog(LL_TRACE, "  Creating Buttons");
  HWND hwndSave =
      buttonMaker(form, 160, ID_BUTTON_SAVE, "Save", BUTTON_ROW);
  HWND hwndCancel =
      buttonMaker(form, 260, ID_BUTTON_CANCEL, "Cancel", BUTTON_ROW);

  MSG msg = {};
  WriteLog(LL_TRACE, "  Polling...");
  while (GetMessage(&msg, NULL, 0, 0)) {
    if (!IsDialogMessage(form, &msg)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }
  WriteLog(LL_TRACE, "  Polling completed");

  WriteLog(LL_TRACE, "  Unregistering class");
  UnregisterClass(className, GetModuleHandle(NULL));
}

DriverConfig DSNForm::getResult() {
  return this->configResult;
}

void DSNForm::updateConfig(DriverConfig config) {
  this->configResult = config;
}

void DSNForm::setReadOnlyDSN() {
  this->readOnlyDSN = true;
}
