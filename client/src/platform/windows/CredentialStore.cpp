#ifdef _WIN32

#include "platform/windows/CredentialStore.h"

#include <QCryptographicHash>

#include <windows.h>
#include <wincred.h>

namespace dropspace::windows {
namespace {

QString targetName(const QString& serverUrl, const QString& deviceId) {
  const auto serverHash = QCryptographicHash::hash(serverUrl.toUtf8(), QCryptographicHash::Sha256).toHex();
  return QStringLiteral("DropSpace/%1/%2").arg(QString::fromLatin1(serverHash), deviceId);
}

QString windowsError(DWORD code) {
  wchar_t* message = nullptr;
  const DWORD count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                         FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
  const QString result = count && message ? QString::fromWCharArray(message, count).trimmed()
                                          : QStringLiteral("Windows error %1").arg(code);
  if (message) LocalFree(message);
  return result;
}

}  // namespace

bool writeDeviceToken(const QString& serverUrl, const QString& deviceId,
                      const QByteArray& token, QString* error) {
  if (token.isEmpty() || token.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
    if (error) *error = QStringLiteral("Invalid device token size");
    return false;
  }
  const QString target = targetName(serverUrl, deviceId);
  CREDENTIALW credential{};
  credential.Type = CRED_TYPE_GENERIC;
  credential.TargetName = const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(target.utf16()));
  credential.CredentialBlobSize = static_cast<DWORD>(token.size());
  credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(token.constData()));
  credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
  credential.UserName = const_cast<LPWSTR>(L"DropSpace device token");
  if (!CredWriteW(&credential, 0)) {
    if (error) *error = windowsError(GetLastError());
    return false;
  }
  return true;
}

QByteArray readDeviceToken(const QString& serverUrl, const QString& deviceId, QString* error) {
  const QString target = targetName(serverUrl, deviceId);
  PCREDENTIALW credential = nullptr;
  if (!CredReadW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC, 0, &credential)) {
    if (error) *error = windowsError(GetLastError());
    return {};
  }
  const QByteArray token(reinterpret_cast<const char*>(credential->CredentialBlob),
                         static_cast<qsizetype>(credential->CredentialBlobSize));
  CredFree(credential);
  return token;
}

bool deleteDeviceToken(const QString& serverUrl, const QString& deviceId, QString* error) {
  const QString target = targetName(serverUrl, deviceId);
  if (!CredDeleteW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC, 0) &&
      GetLastError() != ERROR_NOT_FOUND) {
    if (error) *error = windowsError(GetLastError());
    return false;
  }
  return true;
}

}  // namespace dropspace::windows

#endif

