#pragma once

#include <QJsonArray>
#include <QSqlDatabase>
#include <QString>

namespace dropspace {

struct StoredIdentity {
  QString serverUrl;
  QString spaceId;
  QString deviceId;
};

struct LocalSource {
  QString kind;
  QString originalPath;
  QString objectSha256;
  QString treeSha256;
};

class ClientDatabase final {
 public:
  ClientDatabase();
  ~ClientDatabase();

  [[nodiscard]] bool open(QString* error);
  [[nodiscard]] StoredIdentity identity(QString* error) const;
  [[nodiscard]] bool saveIdentity(const StoredIdentity& identity, QString* error);
  [[nodiscard]] bool saveLocalSource(const QString& fileId, const QString& kind,
                                     const QString& originalPath, const QString& objectSha256,
                                     const QString& treeSha256, QString* error);
  [[nodiscard]] bool replaceSnapshot(const QJsonArray& files, qint64 snapshotSeq, QString* error);
  [[nodiscard]] LocalSource localSource(const QString& fileId, QString* error) const;

 private:
  QString connectionName_;
  QSqlDatabase database_;
};

}  // namespace dropspace

