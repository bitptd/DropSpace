#pragma once

#include <QByteArray>
#include <QString>

namespace dropspace::macos {

bool writeDeviceToken(const QString& serverUrl, const QString& deviceId,
                      const QByteArray& token, QString* error);
QByteArray readDeviceToken(const QString& serverUrl, const QString& deviceId,
                           QString* error);
bool deleteDeviceToken(const QString& serverUrl, const QString& deviceId,
                       QString* error);

}  // namespace dropspace::macos
