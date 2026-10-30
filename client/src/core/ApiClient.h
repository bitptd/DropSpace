#pragma once

#include "storage/ClientDatabase.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QQueue>
#include <QSet>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>

#include <memory>
#include <functional>

class QNetworkReply;

namespace dropspace {

class ApiClient final : public QObject {
  Q_OBJECT

 public:
  explicit ApiClient(ClientDatabase* database, QObject* parent = nullptr);

  [[nodiscard]] bool restore(QString* error);
  [[nodiscard]] bool isPaired() const { return !token_.isEmpty(); }
  [[nodiscard]] QString serverUrl() const { return server_.toString(); }
  void pair(const QString& serverUrl, const QString& pairingToken, const QString& displayName);
  void refreshSnapshot();
  void uploadPath(const QString& path);
  void deleteObject(const QJsonObject& file);
  void exportObject(const QJsonObject& file, const QString& destination);
  void acquireReadableObject(
      const QJsonObject& file,
      std::function<void(const QString& path, const QString& error)> completion);
  [[nodiscard]] QString acquireReadableObjectBlocking(const QJsonObject& file,
                                                       QString* error);
  void fetchFolderManifest(
      const QJsonObject& file,
      std::function<void(const QJsonArray& entries, const QString& error)> completion);

 signals:
  void paired(const QString& serverUrl, const QString& deviceId);
  void snapshotReady(const QJsonArray& files);
  void uploadReady(const QString& fileId, const QString& name);
  void statusChanged(const QString& text);
  void failed(const QString& text);
  void cacheUsageChanged(qint64 bytes);
  void exportReady(const QString& destination);

 private:
  struct PreparedUpload;
  [[nodiscard]] bool setEndpoint(const QString& value, QString* error);
  QNetworkRequest request(const QString& relative) const;
  void createIntent(std::shared_ptr<PreparedUpload> upload);
  void startQueuedUploads();
  void finishUploadSlot();
  void putContent(const QString& fileId, std::shared_ptr<PreparedUpload> upload);
  void finishReplyError(QNetworkReply* reply, const QString& context);
  void planPrecache(const QJsonArray& files);
  void startNextPrecache();
  void downloadVerified(const QJsonObject& file, const QString& finalPath,
                        std::function<void(const QString&, const QString&)> completion);
  [[nodiscard]] QString cacheObjectPath(const QJsonObject& file) const;
  [[nodiscard]] QString transferObjectPath(const QJsonObject& file) const;
  void startEventStream(qint64 after);

  ClientDatabase* database_;
  QNetworkAccessManager network_;
  QUrl server_;
  QString deviceId_;
  QString spaceId_;
  QByteArray token_;
  QQueue<QJsonObject> precacheQueue_;
  QSet<QString> precacheSelection_;
  bool precacheActive_ = false;
  QJsonArray pendingPrecacheSnapshot_;
  QNetworkReply* eventsReply_ = nullptr;
  QByteArray eventsBuffer_;
  qint64 lastEventSeq_ = 0;
  int eventReconnectMs_ = 1000;
  QQueue<std::shared_ptr<PreparedUpload>> uploadQueue_;
  int activeUploads_ = 0;
};

}  // namespace dropspace

