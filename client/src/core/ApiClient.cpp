#include "core/ApiClient.h"

#include "core/FolderPacker.h"
#include "core/CachePlanner.h"
#include "core/SafeExtractor.h"
#include "platform/windows/CredentialStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHttpHeaders>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QSaveFile>
#include <QMetaObject>
#include <QMutex>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <QWaitCondition>
#include <QUuid>
#include <QtConcurrent>

namespace dropspace {

namespace {
QString objectRoot(bool cache) {
  const auto location = cache ? QStandardPaths::CacheLocation
                              : QStandardPaths::AppLocalDataLocation;
  return QDir(QStandardPaths::writableLocation(location)).filePath(
      cache ? QStringLiteral("objects") : QStringLiteral("transfers"));
}

QString verifyFile(const QString& path, qint64 expectedSize, const QString& expectedHash) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() != expectedSize)
    return QStringLiteral("对象长度不匹配");
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) return QStringLiteral("对象读取失败");
  if (QString::fromLatin1(hash.result().toHex()).compare(expectedHash, Qt::CaseInsensitive) != 0)
    return QStringLiteral("对象 SHA-256 不匹配");
  return {};
}
}  // namespace

struct ApiClient::PreparedUpload {
  QString originalPath;
  QString contentPath;
  QString stagingDirectory;
  QString name;
  QString kind;
  QString format;
  QString objectSha256;
  QString treeSha256;
  qint64 objectBytes = 0;
  qint64 expandedBytes = 0;
  qint64 entryCount = 0;
  QString error;
};

ApiClient::ApiClient(ClientDatabase* database, QObject* parent)
    : QObject(parent), database_(database) {}

bool ApiClient::setEndpoint(const QString& value, QString* error) {
  QUrl url(value.trimmed());
  if (!url.isValid() || url.host().isEmpty() || url.scheme() != QStringLiteral("https")) {
    if (error) *error = QStringLiteral("Server URL must be a valid HTTPS URL");
    return false;
  }
  url.setPath(QString());
  url.setQuery(QString());
  url.setFragment(QString());
  server_ = url;
  return true;
}

bool ApiClient::restore(QString* error) {
  const auto identity = database_->identity(error);
  if (identity.deviceId.isEmpty()) return true;
  if (!setEndpoint(identity.serverUrl, error)) return false;
  deviceId_ = identity.deviceId;
  spaceId_ = identity.spaceId;
#ifdef _WIN32
  token_ = windows::readDeviceToken(identity.serverUrl, identity.deviceId, error);
#endif
  if (token_.isEmpty()) return false;
  refreshSnapshot();
  return true;
}

QNetworkRequest ApiClient::request(const QString& relative) const {
  QUrl url = server_;
  url.setPath(relative);
  QNetworkRequest result(url);
  result.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("DropSpace/0.1 Windows"));
  if (!token_.isEmpty()) result.setRawHeader("Authorization", "Bearer " + token_);
  return result;
}

void ApiClient::pair(const QString& serverUrl, const QString& pairingToken,
                     const QString& displayName) {
  QString error;
  if (!setEndpoint(serverUrl, &error)) {
    emit failed(error);
    return;
  }
  QJsonObject body{{QStringLiteral("pairing_token"), pairingToken.trimmed()},
                   {QStringLiteral("display_name"), displayName.trimmed()},
                   {QStringLiteral("platform"), QStringLiteral("windows")}};
  auto pairRequest = request(QStringLiteral("/v1/pair"));
  pairRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  auto* reply = network_.post(pairRequest, QJsonDocument(body).toJson(QJsonDocument::Compact));
  emit statusChanged(QStringLiteral("正在配对…"));
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    if (reply->error() != QNetworkReply::NoError) {
      finishReplyError(reply, QStringLiteral("配对失败"));
      return;
    }
    const auto object = QJsonDocument::fromJson(reply->readAll()).object();
    const QString deviceId = object.value(QStringLiteral("device_id")).toString();
    const QString spaceId = object.value(QStringLiteral("space_id")).toString();
    const QByteArray token = object.value(QStringLiteral("device_token")).toString().toLatin1();
    QString error;
#ifdef _WIN32
    if (!windows::writeDeviceToken(server_.toString(), deviceId, token, &error)) {
      reply->deleteLater();
      emit failed(QStringLiteral("无法保存设备凭据：%1").arg(error));
      return;
    }
#endif
    if (!database_->saveIdentity({server_.toString(), spaceId, deviceId}, &error)) {
#ifdef _WIN32
      (void)windows::deleteDeviceToken(server_.toString(), deviceId, nullptr);
#endif
      reply->deleteLater();
      emit failed(QStringLiteral("无法保存设备身份：%1").arg(error));
      return;
    }
    deviceId_ = deviceId;
    spaceId_ = spaceId;
    token_ = token;
    reply->deleteLater();
    emit paired(server_.toString(), deviceId_);
    emit statusChanged(QStringLiteral("配对成功"));
    refreshSnapshot();
  });
}

void ApiClient::refreshSnapshot() {
  if (!isPaired()) return;
  auto* reply = network_.get(request(QStringLiteral("/v1/snapshot")));
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    if (reply->error() != QNetworkReply::NoError) {
      finishReplyError(reply, QStringLiteral("同步失败"));
      return;
    }
    const auto object = QJsonDocument::fromJson(reply->readAll()).object();
    const auto files = object.value(QStringLiteral("files")).toArray();
    const qint64 seq = object.value(QStringLiteral("snapshot_seq")).toInteger();
    QString error;
    if (!database_->replaceSnapshot(files, seq, &error)) {
      emit failed(QStringLiteral("本地元数据提交失败：%1").arg(error));
    } else {
      emit snapshotReady(files);
      planPrecache(files);
      startEventStream(seq);
      emit statusChanged(QStringLiteral("已同步 %1 个条目").arg(files.size()));
    }
    reply->deleteLater();
  });
}

void ApiClient::startEventStream(qint64 after) {
  lastEventSeq_ = std::max(lastEventSeq_, after);
  if (eventsReply_) {
    auto* old = eventsReply_; eventsReply_ = nullptr; old->abort();
  }
  auto eventRequest = request(QStringLiteral("/v1/events"));
  QUrl url = eventRequest.url(); QUrlQuery query; query.addQueryItem(QStringLiteral("after"), QString::number(lastEventSeq_));
  url.setQuery(query); eventRequest.setUrl(url);
  eventsBuffer_.clear(); eventsReply_ = network_.get(eventRequest);
  auto* reply = eventsReply_;
  connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
    if (eventsReply_ != reply) return;
    eventsBuffer_ += reply->readAll(); bool changed = false;
    qsizetype boundary;
    while ((boundary = eventsBuffer_.indexOf("\n\n")) >= 0) {
      const QByteArray event = eventsBuffer_.left(boundary); eventsBuffer_.remove(0, boundary + 2);
      for (const auto& line : event.split('\n')) if (line.startsWith("id:")) {
        bool ok = false; const qint64 seq = line.mid(3).trimmed().toLongLong(&ok);
        if (ok && seq > lastEventSeq_) { lastEventSeq_ = seq; changed = true; }
      }
    }
    if (changed) { eventReconnectMs_ = 1000; refreshSnapshot(); }
  });
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    if (eventsReply_ != reply) { reply->deleteLater(); return; }
    eventsReply_ = nullptr; const bool revoked = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 401;
    reply->deleteLater();
    if (revoked) { emit failed(QStringLiteral("设备凭据已撤销，请重新配对")); return; }
    const int delay = eventReconnectMs_; eventReconnectMs_ = std::min(eventReconnectMs_ * 2, 30000);
    QTimer::singleShot(delay, this, [this] { if (isPaired() && !eventsReply_) startEventStream(lastEventSeq_); });
  });
}

QString ApiClient::cacheObjectPath(const QJsonObject& file) const {
  return QDir(objectRoot(true)).filePath(
      file.value(QStringLiteral("object_sha256")).toString().toLower() + QStringLiteral(".object"));
}

QString ApiClient::transferObjectPath(const QJsonObject& file) const {
  return QDir(objectRoot(false)).filePath(
      file.value(QStringLiteral("object_sha256")).toString().toLower() + QStringLiteral(".object"));
}

void ApiClient::planPrecache(const QJsonArray& files) {
  if (precacheActive_) {
    pendingPrecacheSnapshot_ = files;
    return;
  }
  std::vector<CacheCandidate> candidates;
  QHash<QString, QJsonObject> byId;
  const auto now = std::chrono::system_clock::now();
  for (const auto& value : files) {
    const auto object = value.toObject();
    CacheCandidate candidate;
    candidate.id = object.value(QStringLiteral("id")).toString().toStdString();
    candidate.uploadDeviceId = object.value(QStringLiteral("upload_device_id")).toString().toStdString();
    candidate.objectSizeBytes = object.value(QStringLiteral("object_size_bytes")).toInteger(-1);
    candidate.ready = object.value(QStringLiteral("state")).toString() == QStringLiteral("ready");
    candidate.deleted = false;
    const auto created = QDateTime::fromString(object.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs);
    candidate.createdAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(created.toMSecsSinceEpoch()));
    const auto expiresText = object.value(QStringLiteral("expires_at")).toString();
    candidate.expiresAt = expiresText.isEmpty()
        ? std::chrono::system_clock::time_point::max()
        : std::chrono::system_clock::time_point(std::chrono::milliseconds(
              QDateTime::fromString(expiresText, Qt::ISODateWithMs).toMSecsSinceEpoch()));
    candidates.push_back(candidate);
    byId.insert(QString::fromStdString(candidate.id), object);
  }
  const auto plan = dropspace::planPrecache(candidates, deviceId_.toStdString(), now);
  precacheQueue_.clear(); precacheSelection_.clear();
  QSet<QString> retainedPaths;
  for (const auto& id : plan.selectedIds) {
    const auto object = byId.value(QString::fromStdString(id));
    precacheSelection_.insert(QString::fromStdString(id));
    retainedPaths.insert(QFileInfo(cacheObjectPath(object)).fileName());
    precacheQueue_.enqueue(object);
  }
  QDir cache(objectRoot(true)); cache.mkpath(QStringLiteral("."));
  for (const auto& name : cache.entryList({QStringLiteral("*.part"), QStringLiteral("*.object")}, QDir::Files))
    if (name.endsWith(QStringLiteral(".part")) || !retainedPaths.contains(name)) cache.remove(name);
  emit cacheUsageChanged(0);
  startNextPrecache();
}

void ApiClient::startNextPrecache() {
  if (precacheActive_) return;
  while (!precacheQueue_.isEmpty()) {
    const auto file = precacheQueue_.dequeue();
    const QString id = file.value(QStringLiteral("id")).toString();
    if (!precacheSelection_.contains(id)) continue;
    precacheActive_ = true;
    downloadVerified(file, cacheObjectPath(file), [this, id](const QString&, const QString& error) {
      precacheActive_ = false;
      if (!error.isEmpty()) emit statusChanged(QStringLiteral("预缓存失败：%1").arg(error));
      qint64 usage = 0; QDir cache(objectRoot(true));
      for (const auto& info : cache.entryInfoList({QStringLiteral("*.object")}, QDir::Files)) usage += info.size();
      emit cacheUsageChanged(usage);
      if (!pendingPrecacheSnapshot_.isEmpty()) {
        const QJsonArray latest = pendingPrecacheSnapshot_;
        pendingPrecacheSnapshot_ = {};
        planPrecache(latest);
        return;
      }
      startNextPrecache();
    });
    return;
  }
}

void ApiClient::downloadVerified(
    const QJsonObject& file, const QString& finalPath,
    std::function<void(const QString&, const QString&)> completion) {
  const qint64 expectedSize = file.value(QStringLiteral("object_size_bytes")).toInteger();
  const QString expectedHash = file.value(QStringLiteral("object_sha256")).toString();
  if (QFileInfo::exists(finalPath)) {
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, file, finalPath, completion = std::move(completion)]() mutable {
      const QString error = watcher->result(); watcher->deleteLater();
      if (error.isEmpty()) { completion(finalPath, {}); return; }
      QFile::remove(finalPath);
      downloadVerified(file, finalPath, std::move(completion));
    });
    watcher->setFuture(QtConcurrent::run([finalPath, expectedSize, expectedHash] {
      return verifyFile(finalPath, expectedSize, expectedHash);
    }));
    return;
  }
  QDir().mkpath(QFileInfo(finalPath).absolutePath());
  const QString part = finalPath + u'.' + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".part");
  auto output = std::make_shared<QFile>(part);
  if (!output->open(QIODevice::WriteOnly | QIODevice::NewOnly)) { completion({}, output->errorString()); return; }
  auto contentRequest = request(QStringLiteral("/v1/files/%1/content").arg(file.value(QStringLiteral("id")).toString()));
  auto* reply = network_.get(contentRequest);
  auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
  auto received = std::make_shared<qint64>(0);
  auto failed = std::make_shared<QString>();
  connect(reply, &QNetworkReply::readyRead, this, [reply, output, hash, received, failed, expectedSize] {
    const QByteArray bytes = reply->readAll();
    *received += bytes.size();
    if (*received > expectedSize) { *failed = QStringLiteral("服务器响应超过声明长度"); reply->abort(); return; }
    if (output->write(bytes) != bytes.size()) { *failed = output->errorString(); reply->abort(); return; }
    hash->addData(bytes);
  });
  connect(reply, &QNetworkReply::downloadProgress, this, [this, file](qint64 got, qint64 total) {
    emit statusChanged(QStringLiteral("正在下载 %1：%2 / %3 bytes")
        .arg(file.value(QStringLiteral("name")).toString()).arg(got).arg(total));
  });
  connect(reply, &QNetworkReply::finished, this,
          [reply, output, hash, received, failed, expectedSize, expectedHash, finalPath, part,
           completion = std::move(completion)]() mutable {
    output->flush(); output->close();
    QString error = *failed;
    if (error.isEmpty() && reply->error() != QNetworkReply::NoError) error = reply->errorString();
    if (error.isEmpty() && *received != expectedSize) error = QStringLiteral("下载长度不匹配");
    if (error.isEmpty() && QString::fromLatin1(hash->result().toHex()).compare(expectedHash, Qt::CaseInsensitive) != 0)
      error = QStringLiteral("下载 SHA-256 不匹配");
    if (error.isEmpty() && !QFile::rename(part, finalPath)) error = QStringLiteral("无法原子发布下载对象");
    if (!error.isEmpty()) QFile::remove(part);
    reply->deleteLater(); completion(error.isEmpty() ? finalPath : QString(), error);
  });
}

void ApiClient::acquireReadableObject(
    const QJsonObject& file,
    std::function<void(const QString&, const QString&)> completion) {
  const QString fileId = file.value(QStringLiteral("id")).toString();
  const auto source = database_->localSource(fileId, nullptr);
  const QString transfer = transferObjectPath(file);
  if (!source.originalPath.isEmpty()) {
    emit statusChanged(QStringLiteral("正在校验本机原路径…"));
    auto* watcher = new QFutureWatcher<QPair<QString, QString>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, file, transfer, completion = std::move(completion)]() mutable {
      const auto result = watcher->result(); watcher->deleteLater();
      if (result.second.isEmpty()) { completion(result.first, {}); return; }
      downloadVerified(file, transfer, std::move(completion));
    });
    watcher->setFuture(QtConcurrent::run([source, file, transfer] {
      QDir().mkpath(QFileInfo(transfer).absolutePath()); QFile::remove(transfer);
      if (source.kind == QStringLiteral("file")) {
        QFile input(source.originalPath), output(transfer + QStringLiteral(".part"));
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
          return QPair<QString, QString>{QString(), QStringLiteral("原文件不可读")};
        QCryptographicHash hash(QCryptographicHash::Sha256); QByteArray buffer(1024 * 1024, Qt::Uninitialized);
        while (!input.atEnd()) { const qint64 n = input.read(buffer.data(), buffer.size()); if (n <= 0 || output.write(buffer.constData(), n) != n) { output.close(); QFile::remove(output.fileName()); return QPair<QString, QString>{QString(), QStringLiteral("原文件快照失败")}; } hash.addData(QByteArrayView(buffer.constData(), n)); }
        output.close();
        if (QString::fromLatin1(hash.result().toHex()).compare(source.objectSha256, Qt::CaseInsensitive) != 0 ||
            !QFile::rename(output.fileName(), transfer)) { QFile::remove(output.fileName()); return QPair<QString, QString>{QString(), QStringLiteral("原文件已改变")}; }
        return QPair<QString, QString>{transfer, QString()};
      }
      PackedFolder packed; QString error; const QString part = transfer + QStringLiteral(".part");
      if (!packFolder(source.originalPath, part, &packed, &error) ||
          packed.objectSha256.compare(source.objectSha256, Qt::CaseInsensitive) != 0 ||
          !QFile::rename(part, transfer)) { QFile::remove(part); return QPair<QString, QString>{QString(), QStringLiteral("原文件夹已改变或不可读")}; }
      return QPair<QString, QString>{transfer, QString()};
    }));
    return;
  }
  const QString cached = cacheObjectPath(file);
  if (QFileInfo::exists(cached)) { downloadVerified(file, cached, std::move(completion)); return; }
  downloadVerified(file, transfer, std::move(completion));
}

QString ApiClient::acquireReadableObjectBlocking(const QJsonObject& file, QString* error) {
  if (QThread::currentThread() == thread()) {
    if (error) *error = QStringLiteral("阻塞内容获取不能在 UI 线程执行");
    return {};
  }
  QMutex mutex; QWaitCondition ready; bool done = false; QString path, failure;
  QMetaObject::invokeMethod(this, [this, file, &mutex, &ready, &done, &path, &failure] {
    acquireReadableObject(file, [&mutex, &ready, &done, &path, &failure](const QString& p, const QString& e) {
      QMutexLocker lock(&mutex); path = p; failure = e; done = true; ready.wakeAll();
    });
  }, Qt::QueuedConnection);
  QMutexLocker lock(&mutex);
  while (!done) ready.wait(&mutex);
  if (error) *error = failure;
  return path;
}

void ApiClient::fetchFolderManifest(
    const QJsonObject& file,
    std::function<void(const QJsonArray&, const QString&)> completion) {
  auto* reply = network_.get(request(QStringLiteral("/v1/files/%1/manifest")
      .arg(file.value(QStringLiteral("id")).toString())));
  connect(reply, &QNetworkReply::finished, this, [reply, completion = std::move(completion)]() mutable {
    QString error; QJsonArray entries;
    const QByteArray body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
      const auto object = QJsonDocument::fromJson(body).object();
      error = object.value(QStringLiteral("message")).toString(reply->errorString());
    } else entries = QJsonDocument::fromJson(body).object().value(QStringLiteral("entries")).toArray();
    reply->deleteLater(); completion(entries, error);
  });
}

void ApiClient::exportObject(const QJsonObject& file, const QString& destination) {
  acquireReadableObject(file, [this, file, destination](const QString& object, const QString& error) {
    if (!error.isEmpty()) { emit failed(QStringLiteral("获取内容失败：%1").arg(error)); return; }
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, destination] {
      const QString error = watcher->result(); watcher->deleteLater();
      if (error.isEmpty()) { emit statusChanged(QStringLiteral("交付完成")); emit exportReady(destination); }
      else emit failed(error);
    });
    watcher->setFuture(QtConcurrent::run([file, object, destination] {
      if (file.value(QStringLiteral("kind")).toString() == QStringLiteral("folder")) {
        std::vector<ExtractedEntry> manifest; QString error;
        if (!safeExtractZip(object, destination, &manifest, &error)) return QStringLiteral("安全解压失败：%1").arg(error);
        return QString();
      }
      const QString part = destination + QStringLiteral(".dropspace.part"); QFile::remove(part);
      if (!QFile::copy(object, part)) return QStringLiteral("无法写入目标临时文件");
      if (!QFile::rename(part, destination)) { QFile::remove(part); return QStringLiteral("无法发布目标文件"); }
      return QString();
    }));
  });
}

void ApiClient::uploadPath(const QString& path) {
  if (!isPaired()) {
    emit failed(QStringLiteral("请先在设备设置中配对"));
    return;
  }
  const QFileInfo source(path);
  if (!source.exists() || (!source.isFile() && !source.isDir())) {
    emit failed(QStringLiteral("不支持的拖入对象：%1").arg(path));
    return;
  }
  emit statusChanged(source.isDir() ? QStringLiteral("正在扫描并压缩 %1…").arg(source.fileName())
                                    : QStringLiteral("正在计算 %1 的 SHA-256…").arg(source.fileName()));
  auto* watcher = new QFutureWatcher<std::shared_ptr<PreparedUpload>>(this);
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
    auto upload = watcher->result();
    watcher->deleteLater();
    if (!upload->error.isEmpty()) {
      if (!upload->stagingDirectory.isEmpty()) QDir(upload->stagingDirectory).removeRecursively();
      emit failed(upload->error);
      return;
    }
    uploadQueue_.enqueue(std::move(upload));
    startQueuedUploads();
  });
  watcher->setFuture(QtConcurrent::run([path] {
    auto upload = std::make_shared<PreparedUpload>();
    const QFileInfo source(path);
    upload->originalPath = source.absoluteFilePath();
    upload->name = source.fileName();
    if (source.isDir()) {
      const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
      upload->stagingDirectory = QDir(root).filePath(
          QStringLiteral("uploads/%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
      if (!QDir().mkpath(upload->stagingDirectory)) {
        upload->error = QStringLiteral("无法创建文件夹上传暂存区");
        return upload;
      }
      PackedFolder packed;
      QString error;
      const QString zip = QDir(upload->stagingDirectory).filePath(QStringLiteral("object.zip.part"));
      if (!packFolder(path, zip, &packed, &error)) {
        upload->error = error;
        return upload;
      }
      upload->contentPath = zip;
      upload->kind = QStringLiteral("folder");
      upload->format = QStringLiteral("zip");
      upload->objectSha256 = packed.objectSha256;
      upload->treeSha256 = packed.treeSha256;
      upload->objectBytes = packed.objectSizeBytes;
      upload->expandedBytes = packed.expandedSizeBytes;
      upload->entryCount = packed.entryCount;
    } else {
      QFile file(source.absoluteFilePath());
      if (!file.open(QIODevice::ReadOnly)) {
        upload->error = QStringLiteral("无法读取 %1：%2").arg(source.fileName(), file.errorString());
        return upload;
      }
      QCryptographicHash hash(QCryptographicHash::Sha256);
      if (!hash.addData(&file)) {
        upload->error = QStringLiteral("读取 %1 时失败").arg(source.fileName());
        return upload;
      }
      upload->contentPath = source.absoluteFilePath();
      upload->kind = QStringLiteral("file");
      upload->format = QStringLiteral("raw");
      upload->objectSha256 = QString::fromLatin1(hash.result().toHex());
      upload->objectBytes = source.size();
      upload->expandedBytes = source.size();
      upload->entryCount = 1;
    }
    return upload;
  }));
}

void ApiClient::startQueuedUploads() {
  while (activeUploads_ < 2 && !uploadQueue_.isEmpty()) {
    ++activeUploads_;
    createIntent(uploadQueue_.dequeue());
  }
}

void ApiClient::finishUploadSlot() {
  if (activeUploads_ > 0) --activeUploads_;
  startQueuedUploads();
}

void ApiClient::createIntent(std::shared_ptr<PreparedUpload> upload) {
  QJsonObject body{{QStringLiteral("client_upload_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                   {QStringLiteral("kind"), upload->kind},
                   {QStringLiteral("name"), upload->name},
                   {QStringLiteral("object_format"), upload->format},
                   {QStringLiteral("object_size_bytes"), upload->objectBytes},
                   {QStringLiteral("object_sha256"), upload->objectSha256},
                   {QStringLiteral("expanded_size_bytes"), upload->expandedBytes},
                   {QStringLiteral("entry_count"), upload->entryCount}};
  if (!upload->treeSha256.isEmpty()) body.insert(QStringLiteral("tree_sha256"), upload->treeSha256);
  auto intentRequest = request(QStringLiteral("/v1/files"));
  intentRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  auto* reply = network_.post(intentRequest, QJsonDocument(body).toJson(QJsonDocument::Compact));
  emit statusChanged(QStringLiteral("正在创建 %1 的上传任务…").arg(upload->name));
  connect(reply, &QNetworkReply::finished, this, [this, reply, upload] {
    if (reply->error() != QNetworkReply::NoError) {
      finishReplyError(reply, QStringLiteral("创建上传任务失败"));
      if (!upload->stagingDirectory.isEmpty()) QDir(upload->stagingDirectory).removeRecursively();
      finishUploadSlot();
      return;
    }
    const QString fileId = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("id")).toString();
    reply->deleteLater();
    putContent(fileId, upload);
  });
}

void ApiClient::putContent(const QString& fileId, std::shared_ptr<PreparedUpload> upload) {
  auto* file = new QFile(upload->contentPath);
  if (!file->open(QIODevice::ReadOnly)) {
    emit failed(QStringLiteral("打开上传内容失败：%1").arg(file->errorString()));
    delete file;
    if (!upload->stagingDirectory.isEmpty()) QDir(upload->stagingDirectory).removeRecursively();
    finishUploadSlot();
    return;
  }
  auto contentRequest = request(QStringLiteral("/v1/files/%1/content").arg(fileId));
  contentRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
  contentRequest.setHeader(QNetworkRequest::ContentLengthHeader, upload->objectBytes);
  auto* reply = network_.put(contentRequest, file);
  file->setParent(reply);
  emit statusChanged(QStringLiteral("正在上传 %1…").arg(upload->name));
  connect(reply, &QNetworkReply::uploadProgress, this, [this, upload](qint64 sent, qint64 total) {
    emit statusChanged(QStringLiteral("正在上传 %1：%2 / %3 bytes").arg(upload->name).arg(sent).arg(total));
  });
  connect(reply, &QNetworkReply::finished, this, [this, reply, upload, fileId] {
    if (reply->error() != QNetworkReply::NoError) {
      finishReplyError(reply, QStringLiteral("上传失败"));
    } else {
      QString error;
      if (!database_->saveLocalSource(fileId, upload->kind, upload->originalPath,
                                      upload->objectSha256, upload->treeSha256, &error)) {
        emit failed(QStringLiteral("上传成功，但原路径记录失败：%1").arg(error));
      } else {
        emit statusChanged(QStringLiteral("%1 已上传并通过服务端校验").arg(upload->name));
        emit uploadReady(fileId, upload->name);
      }
      reply->deleteLater();
      refreshSnapshot();
    }
    if (!upload->stagingDirectory.isEmpty()) QDir(upload->stagingDirectory).removeRecursively();
    finishUploadSlot();
  });
}

void ApiClient::deleteObject(const QJsonObject& file) {
  const QString id = file.value(QStringLiteral("id")).toString();
  auto* reply = network_.deleteResource(request(QStringLiteral("/v1/files/%1").arg(id)));
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    if (reply->error() != QNetworkReply::NoError) { finishReplyError(reply, QStringLiteral("删除失败")); return; }
    reply->deleteLater(); emit statusChanged(QStringLiteral("云端条目已删除")); refreshSnapshot();
  });
}

void ApiClient::finishReplyError(QNetworkReply* reply, const QString& context) {
  const auto object = QJsonDocument::fromJson(reply->readAll()).object();
  const QString message = object.value(QStringLiteral("message")).toString(reply->errorString());
  emit failed(QStringLiteral("%1：%2").arg(context, message));
  reply->deleteLater();
}

}  // namespace dropspace

