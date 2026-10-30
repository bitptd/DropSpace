#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QKeySequence>
#include <QObject>
#include <QString>

#include <windows.h>

namespace dropspace::windows {

class WindowsClipboard final : public QObject {
  Q_OBJECT

 public:
  explicit WindowsClipboard(QObject* parent = nullptr);
  ~WindowsClipboard() override;

  [[nodiscard]] bool initialize(QString* error);
  [[nodiscard]] bool setMonitoringEnabled(bool enabled, QString* error);
  [[nodiscard]] bool setHistoryHotkey(const QKeySequence& sequence, QString* error);
  [[nodiscard]] bool writeRemote(const QString& clipboardId, const QString& contentHash,
                                 const QString& contentType, const QByteArray& content,
                                 QString* error);

 signals:
  void localContentCaptured(const QString& contentType, const QByteArray& content,
                            const QDateTime& copiedAt);
  void historyHotkeyActivated();
  void clipboardError(const QString& message);

 private:
  static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam,
                                           LPARAM lParam);
  LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
  void captureClipboard(DWORD expectedSequence, int attempt = 0);
  [[nodiscard]] bool readClipboard(QString* contentType, QByteArray* content,
                                   bool* isDropSpaceWrite, QString* error) const;
  [[nodiscard]] bool writeText(const QByteArray& utf8, const QByteArray& marker,
                               QString* error);
  [[nodiscard]] bool writeImage(const QByteArray& pngBytes, const QByteArray& marker,
                                QString* error);

  HWND window_ = nullptr;
  ATOM windowClass_ = 0;
  UINT markerFormat_ = 0;
  UINT pngFormat_ = 0;
  DWORD lastSequence_ = 0;
  bool monitoring_ = false;
  bool hotkeyRegistered_ = false;
  UINT hotkeyModifiers_ = 0;
  UINT hotkeyKey_ = 0;
};

}  // namespace dropspace::windows

