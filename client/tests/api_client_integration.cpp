#include "core/ApiClient.h"
#include "platform/windows/CredentialStore.h"
#include "storage/ClientDatabase.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QJsonObject>

#include <iostream>

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("DropSpaceTests"));
  QCoreApplication::setApplicationName(QStringLiteral("ApiIntegration"));
  QStandardPaths::setTestModeEnabled(true);
  if (argc != 3) {
    std::cerr << "usage: api_client_integration <https-url> <pairing-token>\n";
    return 2;
  }
  QTemporaryDir source;
  if (!source.isValid() || !QDir().mkpath(source.filePath(QStringLiteral("folder/nested/empty")))) return 3;
  QFile regular(source.filePath(QStringLiteral("regular.txt")));
  const QByteArray regularBytes("public HTTPS payload");
  if (!regular.open(QIODevice::WriteOnly | QIODevice::NewOnly) || regular.write(regularBytes) != regularBytes.size()) return 4;
  regular.close();
  QFile nested(source.filePath(QStringLiteral("folder/nested/内容.txt")));
  const QByteArray nestedBytes("folder payload");
  if (!nested.open(QIODevice::WriteOnly | QIODevice::NewOnly) || nested.write(nestedBytes) != nestedBytes.size()) return 5;
  nested.close();

  dropspace::ClientDatabase database;
  QString error;
  if (!database.open(&error)) {
    std::cerr << error.toStdString() << '\n';
    return 6;
  }
  dropspace::ApiClient api(&database);
  const QString server = QString::fromLocal8Bit(argv[1]);
  QString deviceId;
  int uploads = 0;
  int exitCode = 1;
  bool exporting = false;
  QJsonObject regularRemote, folderRemote;
  const QString regularOutput = source.filePath(QStringLiteral("downloaded.txt"));
  const QString folderOutput = source.filePath(QStringLiteral("restored-folder"));
  QObject::connect(&api, &dropspace::ApiClient::failed, &app, [&](const QString& message) {
    std::cerr << message.toStdString() << '\n';
    exitCode = 7;
    app.quit();
  });
  QObject::connect(&api, &dropspace::ApiClient::paired, &app,
                   [&](const QString&, const QString& id) {
                     deviceId = id;
                     api.uploadPath(regular.fileName());
                   });
  QObject::connect(&api, &dropspace::ApiClient::uploadReady, &app,
                   [&](const QString&, const QString&) {
                     ++uploads;
                     if (uploads == 1) api.uploadPath(source.filePath(QStringLiteral("folder")));
                     else api.refreshSnapshot();
                   });
  QObject::connect(&api, &dropspace::ApiClient::snapshotReady, &app,
                   [&](const QJsonArray& files) {
                     if (uploads == 2 && !exporting && files.size() >= 2) {
                       for (const auto& value : files) {
                         const auto file = value.toObject();
                         if (file.value(QStringLiteral("name")) == QStringLiteral("regular.txt")) regularRemote = file;
                         if (file.value(QStringLiteral("name")) == QStringLiteral("folder")) folderRemote = file;
                       }
                       if (regularRemote.isEmpty() || folderRemote.isEmpty()) return;
                       exporting = true;
                       api.fetchFolderManifest(folderRemote, [&](const QJsonArray& entries, const QString& manifestError) {
                         if (!manifestError.isEmpty() || entries.size() != 3) {
                           std::cerr << "manifest: " << manifestError.toStdString() << '\n';
                           exitCode = 11; app.quit(); return;
                         }
                         QFile::remove(regular.fileName());
                         QDir(source.filePath(QStringLiteral("folder"))).removeRecursively();
                         api.exportObject(regularRemote, regularOutput);
                       });
                     }
                   });
  QObject::connect(&api, &dropspace::ApiClient::exportReady, &app,
                   [&](const QString& destination) {
                     if (destination == regularOutput) {
                       QFile output(regularOutput);
                       if (!output.open(QIODevice::ReadOnly) || output.readAll() != regularBytes) {
                         exitCode = 9; app.quit(); return;
                       }
                       api.exportObject(folderRemote, folderOutput);
                     } else if (destination == folderOutput) {
                       QFile output(QDir(folderOutput).filePath(QStringLiteral("nested/内容.txt")));
                       if (!output.open(QIODevice::ReadOnly) || output.readAll() != nestedBytes ||
                           !QFileInfo(QDir(folderOutput).filePath(QStringLiteral("nested/empty"))).isDir()) {
                         exitCode = 10; app.quit(); return;
                       }
                       exitCode = 0; app.quit();
                     }
                   });
  QTimer::singleShot(120000, &app, [&] {
    std::cerr << "timeout\n";
    exitCode = 8;
    app.quit();
  });
  api.pair(server, QString::fromLocal8Bit(argv[2]), QStringLiteral("Windows integration test"));
  app.exec();
#ifdef _WIN32
  if (!deviceId.isEmpty()) (void)dropspace::windows::deleteDeviceToken(server, deviceId, nullptr);
#endif
  if (exitCode == 0) std::cout << "api_client_integration: upload, original-path fallback download, and safe folder restore passed\n";
  return exitCode;
}

