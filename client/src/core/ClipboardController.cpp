#include "core/ClipboardController.h"

#include "core/ApiClient.h"
#include "storage/ClientDatabase.h"

#ifdef _WIN32
#include "platform/windows/WindowsClipboard.h"
#endif

#include <QCryptographicHash>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUuid>

#include <algorithm>

namespace dropspace {

namespace {
constexpr auto kEnabledSetting = "clipboard_enabled";
constexpr auto kHotkeySetting = "clipboard_hotkey";
constexpr auto kLastAppliedSetting = "clipboard_last_applied_sequence";
constexpr qint64 kMaxClipboardDownload = 16 * 1024 * 1024;
constexpr qint64 kMaxClipboardText = 1024 * 1024;
}

ClipboardController::ClipboardController(ApiClient* api, ClientDatabase* database, QObject* parent)
    : QObject(parent), api_(api), database_(database) {
  reconnectTimer_.setSingleShot(true);
  connect(&reconnectTimer_, &QTimer::timeout, this, &ClipboardController::connectWebSocket);
  connect(&socket_, &QWebSocket::connected, this, [this] {
    reconnectMs_ = 1000;
    emit statusChanged(QStringLiteral("共享剪贴板已连接"));
    refreshHistory();
  });
  connect(&socket_, &QWebSocket::textMessageReceived, this,
          &ClipboardController::handleWebSocketMessage);
  connect(&socket_, &QWebSocket::disconnected, this, [this] {
    if (enabled_) {
      onlineDevices_ = 0;
      emit onlineDevicesChanged(onlineDevices_);
      emit statusChanged(QStringLiteral("共享剪贴板连接中断，正在重连"));
      scheduleReconnect();
    }
  });
  connect(api_, &ApiClient::paired, this, [this] {
    if (enabled_) connectWebSocket();
  });
}

ClipboardController::~ClipboardController() {
  reconnectTimer_.stop();
  socket_.close();
}

bool ClipboardController::initialize(QString* error) {
  const QString enabled = database_->setting(QString::fromLatin1(kEnabledSetting),
                                             QStringLiteral("0"), error);
  if (error && !error->isEmpty()) return false;
  const QString shortcut = database_->setting(QString::fromLatin1(kHotkeySetting),
                                              QStringLiteral("Ctrl+Shift+V"), error);
  if (error && !error->isEmpty()) return false;
  bool ok = false;
  lastAppliedSequence_ = database_->setting(QString::fromLatin1(kLastAppliedSetting),
                                            QStringLiteral("0"), error).toLongLong(&ok);
  if (!ok) lastAppliedSequence_ = 0;
  hotkey_ = QKeySequence(shortcut);
  if (hotkey_.isEmpty()) hotkey_ = QKeySequence(QStringLiteral("Ctrl+Shift+V"));

#ifdef _WIN32
  native_ = new windows::WindowsClipboard(this);
  if (!native_->initialize(error)) return false;
  QString hotkeyError;
  if (!native_->setHistoryHotkey(hotkey_, &hotkeyError)) {
    emit statusChanged(QStringLiteral("共享剪贴板可用，但历史快捷键未注册：%1").arg(hotkeyError));
  }
  connect(native_, &windows::WindowsClipboard::localContentCaptured, this,
          &ClipboardController::uploadLocal);
  connect(native_, &windows::WindowsClipboard::historyHotkeyActivated, this,
          &ClipboardController::historyHotkeyActivated);
  connect(native_, &windows::WindowsClipboard::clipboardError, this,
          [this](const QString& message) { emit statusChanged(message); });
#endif
  enabled_ = enabled == QStringLiteral("1");
#ifdef _WIN32
  if (!native_->setMonitoringEnabled(enabled_, error)) return false;
#endif
  if (enabled_ && api_->isPaired()) connectWebSocket();
  return true;
}

void ClipboardController::setEnabled(bool enabled) {
  if (enabled_ == enabled) return;
  QString error;
#ifdef _WIN32
  if (!native_->setMonitoringEnabled(enabled, &error)) {
    emit failed(error);
    return;
  }
#endif
  if (!database_->setSetting(QString::fromLatin1(kEnabledSetting), enabled ? QStringLiteral("1") : QStringLiteral("0"), &error)) {
    QString ignored;
#ifdef _WIN32
    (void)native_->setMonitoringEnabled(enabled_, &ignored);
#endif
    emit failed(QStringLiteral("无法保存共享剪贴板设置：%1").arg(error));
    return;
  }
  enabled_ = enabled;
  if (enabled_) {
    connectWebSocket();
  } else {
    reconnectTimer_.stop();
    socket_.close();
    onlineDevices_ = 0;
    emit onlineDevicesChanged(onlineDevices_);
    emit statusChanged(QStringLiteral("共享剪贴板已关闭"));
  }
  emit enabledChanged(enabled_);
}

void ClipboardController::setHotkey(const QKeySequence& sequence) {
  QString error;
#ifdef _WIN32
  if (!native_->setHistoryHotkey(sequence, &error)) {
    emit failed(error);
    return;
  }
#endif
  if (!database_->setSetting(QString::fromLatin1(kHotkeySetting), sequence.toString(), &error)) {
    emit failed(QStringLiteral("无法保存快捷键：%1").arg(error));
    return;
  }
  hotkey_ = sequence;
  emit statusChanged(QStringLiteral("剪贴板历史快捷键已更新为 %1").arg(hotkey_.toString()));
}

void ClipboardController::uploadLocal(const QString& contentType, const QByteArray& content,
                                      const QDateTime& copiedAt) {
  if (!enabled_ || !api_->isPaired()) return;
  const qint64 limit = contentType == QStringLiteral("text") ? kMaxClipboardText
                                                               : kMaxClipboardDownload;
  if (content.size() > limit) {
    emit statusChanged(contentType == QStringLiteral("text")
                           ? QStringLiteral("剪贴板文字超过 1 MiB，未同步")
                           : QStringLiteral("剪贴板图片超过 16 MiB，未同步"));
    return;
  }
  const QString clipboardId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QString hash = QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
  const QJsonObject metadata{
      {QStringLiteral("clipboard_protocol_version"), 1},
      {QStringLiteral("clipboard_id"), clipboardId},
      {QStringLiteral("content_type"), contentType},
      {QStringLiteral("copied_at"), copiedAt.toUTC().toString(Qt::ISODateWithMs)},
      {QStringLiteral("content_hash"), hash},
      {QStringLiteral("content_size"), content.size()}};
  auto* multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
  QHttpPart metadataPart;
  metadataPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                         QStringLiteral("form-data; name=\"metadata\""));
  metadataPart.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  metadataPart.setBody(QJsonDocument(metadata).toJson(QJsonDocument::Compact));
  multipart->append(metadataPart);
  QHttpPart contentPart;
  contentPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                        QStringLiteral("form-data; name=\"content\"; filename=\"clipboard.bin\""));
  contentPart.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
  contentPart.setBody(content);
  multipart->append(contentPart);
  auto* reply = network_.post(api_->authenticatedRequest(QStringLiteral("/v1/clipboard")), multipart);
  multipart->setParent(reply);
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    if (reply->error() != QNetworkReply::NoError) {
      const auto errorObject = QJsonDocument::fromJson(reply->readAll()).object();
      emit failed(QStringLiteral("共享剪贴板上传失败：%1")
                      .arg(errorObject.value(QStringLiteral("message")).toString(reply->errorString())));
    } else {
      emit statusChanged(QStringLiteral("本机剪贴板已同步"));
      refreshHistory();
    }
    reply->deleteLater();
  });
}

void ClipboardController::connectWebSocket() {
  if (!enabled_ || !api_->isPaired() || socket_.state() == QAbstractSocket::ConnectedState ||
      socket_.state() == QAbstractSocket::ConnectingState) return;
  reconnectTimer_.stop();
  QNetworkRequest request = api_->authenticatedRequest(QStringLiteral("/v1/clipboard/ws"));
  QUrl url = request.url();
  url.setScheme(url.scheme() == QStringLiteral("https") ? QStringLiteral("wss")
                                                          : QStringLiteral("ws"));
  request.setUrl(url);
  socket_.open(request);
}

void ClipboardController::scheduleReconnect() {
  if (!enabled_ || reconnectTimer_.isActive()) return;
  reconnectTimer_.start(reconnectMs_);
  reconnectMs_ = std::min(reconnectMs_ * 2, 30000);
}

void ClipboardController::handleWebSocketMessage(const QString& message) {
  const QJsonObject object = QJsonDocument::fromJson(message.toUtf8()).object();
  if (object.value(QStringLiteral("clipboard_protocol_version")).toInt() != 1) return;
  const QString type = object.value(QStringLiteral("type")).toString();
  if (object.contains(QStringLiteral("online_devices"))) {
    onlineDevices_ = object.value(QStringLiteral("online_devices")).toInt();
    emit onlineDevicesChanged(onlineDevices_);
  }
  if (type == QStringLiteral("clipboard.presence")) return;
  if (type == QStringLiteral("clipboard.snapshot")) {
    const QJsonArray items = object.value(QStringLiteral("items")).toArray();
    emit historyReady(items);
    applyLatestIfNeeded(items);
    return;
  }
  // A notification can arrive later than another device's copy event. Always
  // reload the authoritative copied_at ordering before changing the clipboard.
  refreshHistory();
}

void ClipboardController::refreshHistory() {
  if (!api_->isPaired()) {
    emit failed(QStringLiteral("请先完成设备配对"));
    return;
  }
  auto* reply = network_.get(api_->authenticatedRequest(QStringLiteral("/v1/clipboard/history?limit=20")));
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    if (reply->error() != QNetworkReply::NoError) {
      emit statusChanged(QStringLiteral("剪贴板历史获取失败：%1").arg(reply->errorString()));
      reply->deleteLater();
      return;
    }
    const QJsonArray items = QJsonDocument::fromJson(reply->readAll()).object()
                                 .value(QStringLiteral("items")).toArray();
    emit historyReady(items);
    applyLatestIfNeeded(items);
    reply->deleteLater();
  });
}

void ClipboardController::applyLatestIfNeeded(const QJsonArray& items) {
  if (!enabled_ || items.isEmpty()) return;
  const QJsonObject latest = items.first().toObject();
  const qint64 sequence = latest.value(QStringLiteral("server_sequence")).toInteger();
  if (sequence <= lastAppliedSequence_ ||
      latest.value(QStringLiteral("source_device_id")).toString() == api_->deviceId()) return;
  applyHistoryItem(latest);
}

void ClipboardController::applyHistoryItem(const QJsonObject& item) {
  if (!enabled_) {
    emit failed(QStringLiteral("请先开启共享剪贴板"));
    return;
  }
  downloadContent(item, [this, item](const QByteArray& bytes, const QString& error) {
    if (!error.isEmpty()) {
      emit failed(error);
      return;
    }
    QString writeError;
#ifdef _WIN32
    if (!native_->writeRemote(item.value(QStringLiteral("clipboard_id")).toString(),
                              item.value(QStringLiteral("content_hash")).toString(),
                              item.value(QStringLiteral("content_type")).toString(), bytes,
                              &writeError)) {
      emit failed(QStringLiteral("更新系统剪贴板失败：%1").arg(writeError));
      return;
    }
#endif
    const qint64 sequence = item.value(QStringLiteral("server_sequence")).toInteger();
    persistLastApplied(sequence);
    emit remoteClipboardApplied(item.value(QStringLiteral("content_type")).toString());
    emit statusChanged(QStringLiteral("远端内容已写入系统剪贴板，可直接粘贴"));
  });
}

void ClipboardController::downloadContent(
    const QJsonObject& item,
    std::function<void(const QByteArray&, const QString&)> completion) {
  const QString reference = item.value(QStringLiteral("content_reference")).toString();
  auto* reply = network_.get(api_->authenticatedRequest(reference));
  connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 received, qint64 total) {
    if (received > kMaxClipboardDownload || total > kMaxClipboardDownload)
      reply->abort();
  });
  connect(reply, &QNetworkReply::finished, this, [reply, item, completion = std::move(completion)]() mutable {
    if (reply->error() != QNetworkReply::NoError) {
      completion({}, QStringLiteral("剪贴板内容下载失败：%1").arg(reply->errorString()));
      reply->deleteLater();
      return;
    }
    const QByteArray bytes = reply->readAll();
    const qint64 expectedSize = item.value(QStringLiteral("content_size")).toInteger();
    const QByteArray expectedHash = item.value(QStringLiteral("content_hash")).toString().toLatin1().toLower();
    const QByteArray actualHash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    if (bytes.size() > kMaxClipboardDownload || bytes.size() != expectedSize || actualHash != expectedHash) {
      completion({}, QStringLiteral("剪贴板内容完整性校验失败"));
    } else {
      completion(bytes, {});
    }
    reply->deleteLater();
  });
}

void ClipboardController::downloadThumbnail(
    const QJsonObject& item,
    std::function<void(const QByteArray&, const QString&)> completion) {
  const QString reference = item.value(QStringLiteral("thumbnail")).toString();
  if (reference.isEmpty()) {
    completion({}, QStringLiteral("没有缩略图"));
    return;
  }
  auto* reply = network_.get(api_->authenticatedRequest(reference));
  connect(reply, &QNetworkReply::finished, this, [reply, completion = std::move(completion)]() mutable {
    if (reply->error() == QNetworkReply::NoError) completion(reply->readAll(), {});
    else completion({}, reply->errorString());
    reply->deleteLater();
  });
}

void ClipboardController::deleteHistoryItem(const QJsonObject& item) {
  const QString id = item.value(QStringLiteral("clipboard_id")).toString();
  auto* reply = network_.deleteResource(api_->authenticatedRequest(QStringLiteral("/v1/clipboard/%1").arg(id)));
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    if (reply->error() != QNetworkReply::NoError) emit failed(QStringLiteral("删除剪贴板历史失败：%1").arg(reply->errorString()));
    else refreshHistory();
    reply->deleteLater();
  });
}

void ClipboardController::clearHistory() {
  auto* reply = network_.deleteResource(api_->authenticatedRequest(QStringLiteral("/v1/clipboard")));
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    if (reply->error() != QNetworkReply::NoError) emit failed(QStringLiteral("清空剪贴板历史失败：%1").arg(reply->errorString()));
    else {
      emit historyReady({});
      emit statusChanged(QStringLiteral("共享剪贴板历史已清空"));
    }
    reply->deleteLater();
  });
}

void ClipboardController::persistLastApplied(qint64 sequence) {
  lastAppliedSequence_ = std::max(lastAppliedSequence_, sequence);
  QString error;
  if (!database_->setSetting(QString::fromLatin1(kLastAppliedSetting),
                             QString::number(lastAppliedSequence_), &error)) {
    emit statusChanged(QStringLiteral("无法保存剪贴板同步游标：%1").arg(error));
  }
}

}  // namespace dropspace

