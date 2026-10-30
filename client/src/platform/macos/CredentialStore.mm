#include "platform/macos/CredentialStore.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

namespace dropspace::macos {
namespace {

NSString* text(const QString& value) {
  return [NSString stringWithCharacters:reinterpret_cast<const unichar*>(value.utf16())
                                  length:static_cast<NSUInteger>(value.size())];
}

NSString* service(const QString& serverUrl) {
  return [@"com.dropspace.client.device-token:" stringByAppendingString:text(serverUrl)];
}

NSMutableDictionary* query(const QString& serverUrl, const QString& deviceId) {
  return [@{
    (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
    (__bridge id)kSecAttrService : service(serverUrl),
    (__bridge id)kSecAttrAccount : text(deviceId),
  } mutableCopy];
}

QString statusMessage(OSStatus status) {
  CFStringRef description = SecCopyErrorMessageString(status, nullptr);
  if (!description) return QStringLiteral("Keychain error %1").arg(status);
  NSString* message = CFBridgingRelease(description);
  return QString::fromNSString(message);
}

void setError(QString* error, OSStatus status) {
  if (error) *error = statusMessage(status);
}

}  // namespace

bool writeDeviceToken(const QString& serverUrl, const QString& deviceId,
                      const QByteArray& token, QString* error) {
  if (error) error->clear();
  if (serverUrl.isEmpty() || deviceId.isEmpty() || token.isEmpty()) {
    if (error) *error = QStringLiteral("Invalid empty Keychain credential");
    return false;
  }
  NSMutableDictionary* item = query(serverUrl, deviceId);
  NSData* data = [NSData dataWithBytes:token.constData()
                                length:static_cast<NSUInteger>(token.size())];
  OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)item,
                                  (__bridge CFDictionaryRef)@{
                                    (__bridge id)kSecValueData : data,
                                  });
  if (status == errSecItemNotFound) {
    item[(__bridge id)kSecValueData] = data;
    item[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
    status = SecItemAdd((__bridge CFDictionaryRef)item, nullptr);
  }
  if (status != errSecSuccess) {
    setError(error, status);
    return false;
  }
  return true;
}

QByteArray readDeviceToken(const QString& serverUrl, const QString& deviceId,
                           QString* error) {
  if (error) error->clear();
  NSMutableDictionary* item = query(serverUrl, deviceId);
  item[(__bridge id)kSecReturnData] = @YES;
  item[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
  CFTypeRef result = nullptr;
  const OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)item, &result);
  if (status == errSecItemNotFound) {
    if (error) *error = QStringLiteral("Device token is not present in macOS Keychain");
    return {};
  }
  if (status != errSecSuccess) {
    setError(error, status);
    return {};
  }
  NSData* data = CFBridgingRelease(result);
  return QByteArray(static_cast<const char*>(data.bytes), static_cast<qsizetype>(data.length));
}

bool deleteDeviceToken(const QString& serverUrl, const QString& deviceId,
                       QString* error) {
  if (error) error->clear();
  const OSStatus status = SecItemDelete((__bridge CFDictionaryRef)query(serverUrl, deviceId));
  if (status != errSecSuccess && status != errSecItemNotFound) {
    setError(error, status);
    return false;
  }
  return true;
}

}  // namespace dropspace::macos
