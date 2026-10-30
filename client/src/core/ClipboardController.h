#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QKeySequence>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QWebSocket>

#include <functional>

namespace dropspace {

class ApiClient;
class ClientDatabase;

#ifdef _WIN32
namespace windows { class WindowsClipboard; }
#endif

class ClipboardController final : public QObject {
  Q_OBJECT

 public:
  explicit ClipboardController(ApiClient* api, ClientDatabase* database,
                               QObject* parent = nullptr);
  ~ClipboardController() override;

  [[nodiscard]] bool initialize(QString* error);
  [[nodiscard]] bool enabled() const { return enabled_; }
  [[nodiscard]] QKeySequence hotkey() const { return hotkey_; }
  [[nodiscard]] int onlineDevices() const { return onlineDevices_; }
  void setEnabled(bool enabled);
  void setHotkey(const QKeySequence& sequence);
  void refreshHistory();
  void applyHistoryItem(const QJsonObject& item);
  void deleteHistoryItem(const QJsonObject& item);
  void clearHistory();
  void downloadThumbnail(const QJsonObject& item,
                         std::function<void(const QByteArray&, const QString&)> completion);

 signals:
  void enabledChanged(bool enabled);
  void statusChanged(const QString& status);
  void failed(const QString& message);
  void historyReady(const QJsonArray& items);
  void historyHotkeyActivated();
  void remoteClipboardApplied(const QString& contentType);
  void onlineDevicesChanged(int count);

 private:
  void uploadLocal(const QString& contentType, const QByteArray& content,
                   const QDateTime& copiedAt);
  void connectWebSocket();
  void scheduleReconnect();
  void handleWebSocketMessage(const QString& message);
  void applyLatestIfNeeded(const QJsonArray& items);
  void downloadContent(const QJsonObject& item,
                       std::function<void(const QByteArray&, const QString&)> completion);
  void persistLastApplied(qint64 sequence);

  ApiClient* api_;
  ClientDatabase* database_;
  QNetworkAccessManager network_;
  QWebSocket socket_;
  QTimer reconnectTimer_;
#ifdef _WIN32
  windows::WindowsClipboard* native_ = nullptr;
#endif
  bool enabled_ = false;
  QKeySequence hotkey_{QStringLiteral("Ctrl+Shift+V")};
  qint64 lastAppliedSequence_ = 0;
  int reconnectMs_ = 1000;
  int onlineDevices_ = 0;
};

}  // namespace dropspace

