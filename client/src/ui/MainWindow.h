#pragma once

#include <QMainWindow>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

#include <memory>

class QLabel;
class QListWidget;
class QCloseEvent;
class QEvent;
class QTimer;

namespace dropspace {

class ApiClient;
class ClientDatabase;

class MainWindow final : public QMainWindow {
 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;
  void showFullWindow();
  void showTrayPopup(const QRect& trayGeometry);

 protected:
  void changeEvent(QEvent* event) override;
  void closeEvent(QCloseEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;

 private:
  enum class SortMode { Name, Size, Type, UploadedAt };
  void applySort();
  void applyView();
  void prepareForDrag(const QJsonObject& file);
  void updatePreparedSelectionStatus();

  QListWidget* files_ = nullptr;
  QLabel* status_ = nullptr;
  QWidget* actionBar_ = nullptr;
  std::unique_ptr<ClientDatabase> database_;
  ApiClient* api_ = nullptr;
  QHash<QString, QJsonArray> folderManifests_;
  QHash<QString, QString> preparedDragObjects_;
  QHash<QString, QString> preparedDragFolders_;
  QSet<QString> preparingDragIds_;
  SortMode sortMode_ = SortMode::Name;
  bool sortAscending_ = true;
  bool iconView_ = true;
  bool trayPopup_ = false;
  QTimer* trayDismissTimer_ = nullptr;
  QRect normalGeometry_;
};

}  // namespace dropspace

