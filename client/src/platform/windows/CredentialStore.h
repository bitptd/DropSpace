#pragma once

#include <QByteArray>
#include <QString>

namespace dropspace::windows {

[[nodiscard]] bool writeDeviceToken(const QString& serverUrl, const QString& deviceId,
                                    const QByteArray& token, QString* error);
[[nodiscard]] QByteArray readDeviceToken(const QString& serverUrl, const QString& deviceId,
                                         QString* error);
[[nodiscard]] bool deleteDeviceToken(const QString& serverUrl, const QString& deviceId,
                                     QString* error);

}  // namespace dropspace::windows

