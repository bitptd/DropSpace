#include "storage/ClientDatabase.h"

#include <QDir>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

namespace dropspace {
namespace {

bool execute(QSqlQuery* query, const QString& sql, QString* error) {
  if (query->exec(sql)) return true;
  if (error) *error = query->lastError().text();
  return false;
}

}  // namespace

ClientDatabase::ClientDatabase()
    : connectionName_(QStringLiteral("dropspace-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces))) {}

ClientDatabase::~ClientDatabase() {
  if (database_.isValid()) database_.close();
  database_ = {};
  QSqlDatabase::removeDatabase(connectionName_);
}

bool ClientDatabase::open(QString* error) {
  const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (!QDir().mkpath(root)) {
    if (error) *error = QStringLiteral("Cannot create application data directory");
    return false;
  }
  database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
  database_.setDatabaseName(QDir(root).filePath(QStringLiteral("metadata.sqlite")));
  if (!database_.open()) {
    if (error) *error = database_.lastError().text();
    return false;
  }
  QSqlQuery query(database_);
  const QStringList statements = {
      QStringLiteral("PRAGMA journal_mode=WAL"),
      QStringLiteral(R"SQL(CREATE TABLE IF NOT EXISTS device_identity(
  singleton INTEGER PRIMARY KEY CHECK(singleton=1), server_url TEXT NOT NULL,
  space_id TEXT NOT NULL, device_id TEXT NOT NULL
))SQL"),
      QStringLiteral(R"SQL(CREATE TABLE IF NOT EXISTS remote_files(
  file_id TEXT PRIMARY KEY, upload_device_id TEXT NOT NULL, kind TEXT NOT NULL,
  name TEXT NOT NULL, object_size_bytes INTEGER NOT NULL, object_sha256 TEXT NOT NULL,
  expanded_size_bytes INTEGER NOT NULL, entry_count INTEGER NOT NULL,
  tree_sha256 TEXT, state TEXT NOT NULL, created_at TEXT NOT NULL, expires_at TEXT
))SQL"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS sync_state(singleton INTEGER PRIMARY KEY CHECK(singleton=1), last_applied_seq INTEGER NOT NULL)"),
      QStringLiteral(R"SQL(CREATE TABLE IF NOT EXISTS local_sources(
  file_id TEXT PRIMARY KEY, kind TEXT NOT NULL, original_path TEXT NOT NULL,
  uploaded_object_sha256 TEXT NOT NULL, tree_sha256 TEXT
))SQL")};
  for (const auto& statement : statements) {
    if (!execute(&query, statement, error)) return false;
  }
  return true;
}

StoredIdentity ClientDatabase::identity(QString* error) const {
  QSqlQuery query(database_);
  if (!query.exec(QStringLiteral("SELECT server_url,space_id,device_id FROM device_identity WHERE singleton=1"))) {
    if (error) *error = query.lastError().text();
    return {};
  }
  if (!query.next()) return {};
  return {query.value(0).toString(), query.value(1).toString(), query.value(2).toString()};
}

bool ClientDatabase::saveIdentity(const StoredIdentity& identity, QString* error) {
  QSqlQuery query(database_);
  query.prepare(QStringLiteral("INSERT OR REPLACE INTO device_identity(singleton,server_url,space_id,device_id) VALUES(1,?,?,?)"));
  query.addBindValue(identity.serverUrl);
  query.addBindValue(identity.spaceId);
  query.addBindValue(identity.deviceId);
  if (query.exec()) return true;
  if (error) *error = query.lastError().text();
  return false;
}

bool ClientDatabase::saveLocalSource(const QString& fileId, const QString& kind,
                                     const QString& originalPath, const QString& objectSha256,
                                     const QString& treeSha256, QString* error) {
  QSqlQuery query(database_);
  query.prepare(QStringLiteral("INSERT OR REPLACE INTO local_sources(file_id,kind,original_path,uploaded_object_sha256,tree_sha256) VALUES(?,?,?,?,?)"));
  query.addBindValue(fileId);
  query.addBindValue(kind);
  query.addBindValue(originalPath);
  query.addBindValue(objectSha256);
  query.addBindValue(treeSha256.isEmpty() ? QVariant{} : treeSha256);
  if (query.exec()) return true;
  if (error) *error = query.lastError().text();
  return false;
}

bool ClientDatabase::replaceSnapshot(const QJsonArray& files, qint64 snapshotSeq, QString* error) {
  if (!database_.transaction()) {
    if (error) *error = database_.lastError().text();
    return false;
  }
  QSqlQuery query(database_);
  if (!query.exec(QStringLiteral("DELETE FROM remote_files"))) goto failed;
  query.prepare(QStringLiteral(R"SQL(
INSERT INTO remote_files(file_id,upload_device_id,kind,name,object_size_bytes,object_sha256,
 expanded_size_bytes,entry_count,tree_sha256,state,created_at,expires_at)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?)
)SQL"));
  for (const auto& value : files) {
    const auto object = value.toObject();
    query.bindValue(0, object.value(QStringLiteral("id")).toString());
    query.bindValue(1, object.value(QStringLiteral("upload_device_id")).toString());
    query.bindValue(2, object.value(QStringLiteral("kind")).toString());
    query.bindValue(3, object.value(QStringLiteral("name")).toString());
    query.bindValue(4, object.value(QStringLiteral("object_size_bytes")).toInteger());
    query.bindValue(5, object.value(QStringLiteral("object_sha256")).toString());
    query.bindValue(6, object.value(QStringLiteral("expanded_size_bytes")).toInteger());
    query.bindValue(7, object.value(QStringLiteral("entry_count")).toInteger());
    query.bindValue(8, object.value(QStringLiteral("tree_sha256")).toVariant());
    query.bindValue(9, object.value(QStringLiteral("state")).toString());
    query.bindValue(10, object.value(QStringLiteral("created_at")).toString());
    query.bindValue(11, object.value(QStringLiteral("expires_at")).toVariant());
    if (!query.exec()) goto failed;
  }
  query.prepare(QStringLiteral("INSERT OR REPLACE INTO sync_state(singleton,last_applied_seq) VALUES(1,?)"));
  query.addBindValue(snapshotSeq);
  if (!query.exec()) goto failed;
  if (database_.commit()) return true;
failed:
  if (error) *error = query.lastError().text();
  database_.rollback();
  return false;
}

LocalSource ClientDatabase::localSource(const QString& fileId, QString* error) const {
  QSqlQuery query(database_);
  query.prepare(QStringLiteral("SELECT kind,original_path,uploaded_object_sha256,tree_sha256 FROM local_sources WHERE file_id=?"));
  query.addBindValue(fileId);
  if (!query.exec()) {
    if (error) *error = query.lastError().text();
    return {};
  }
  if (!query.next()) return {};
  return {query.value(0).toString(), query.value(1).toString(),
          query.value(2).toString(), query.value(3).toString()};
}

}  // namespace dropspace

