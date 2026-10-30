#include "ui/MainWindow.h"

#include "core/ApiClient.h"
#include "core/FolderPacker.h"
#include "core/SafeExtractor.h"
#include "storage/ClientDatabase.h"

#include <QApplication>
#include <QActionGroup>
#include <QCollator>
#include <QCloseEvent>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QListWidget>
#include <QLineEdit>
#include <QMimeData>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMenu>
#include <QStyle>
#include <QPushButton>
#include <QStatusBar>
#include <QScreen>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QStandardPaths>
#include <QUuid>
#include <QtConcurrent>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <functional>
#include <thread>
#include <mutex>

#ifdef _WIN32
#include "platform/windows/VirtualFileDrag.h"
#include <windows.h>
#elif defined(__APPLE__)
#include "platform/macos/FilePromiseDrag.h"
#endif

namespace dropspace {

namespace {

#ifdef __APPLE__
bool p1PromiseMode() {
  return QCoreApplication::arguments().contains(QStringLiteral("--p1-file-promises"));
}

void appendP1PromiseRows(QListWidget* files) {
  if (!p1PromiseMode()) return;
  auto add = [files](const QString& name, const QString& kind) {
    auto* item = new QListWidgetItem(
        files->style()->standardIcon(kind == QStringLiteral("folder")
                                         ? QStyle::SP_DirIcon : QStyle::SP_FileIcon),
        name, files);
    item->setData(Qt::UserRole, QVariantMap{
        {QStringLiteral("p1_demo"), kind},
        {QStringLiteral("name"), name},
        {QStringLiteral("kind"), kind},
    });
    item->setToolTip(kind == QStringLiteral("folder")
        ? QStringLiteral("P1：放到 Finder 后后台等待 15 秒，再由 SafeExtractor 交付真实文件夹")
        : QStringLiteral("P1：放到 Finder 后后台等待 3 秒，再交付真实文件"));
  };
  add(QStringLiteral("DropSpace-delay-3s.txt"), QStringLiteral("file"));
  add(QStringLiteral("DropSpace 文件夹"), QStringLiteral("folder"));
}
#endif

class NativeDragListWidget final : public QListWidget {
 public:
  using QListWidget::QListWidget;
  std::function<void()> startNativeDrag;

 protected:
  void mousePressEvent(QMouseEvent* event) override {
    auto* clicked = itemAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && clicked && clicked->isSelected() &&
        selectedItems().size() > 1 && event->modifiers() == Qt::NoModifier) {
      setCurrentItem(clicked, QItemSelectionModel::NoUpdate);
      armed_ = true;
      pressPosition_ = event->position().toPoint();
      event->accept();
      return;
    }
    QListWidget::mousePressEvent(event);
    armed_ = event->button() == Qt::LeftButton && currentItem() &&
             itemAt(event->position().toPoint()) == currentItem();
    pressPosition_ = event->position().toPoint();
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (armed_ && (event->buttons() & Qt::LeftButton) &&
        (event->position().toPoint() - pressPosition_).manhattanLength() >=
            QApplication::startDragDistance()) {
      armed_ = false;
      if (currentItem() && startNativeDrag) startNativeDrag();
      return;
    }
    QListWidget::mouseMoveEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    armed_ = false;
    QListWidget::mouseReleaseEvent(event);
  }

  void startDrag(Qt::DropActions) override {
    if (currentItem() && startNativeDrag) startNativeDrag();
  }

 private:
  QPoint pressPosition_;
  bool armed_ = false;
};

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
#ifdef __APPLE__
  macos::installFilePromiseEventMonitor();
#endif
  setWindowTitle(QStringLiteral("DropSpace"));
  resize(760, 480);

  trayDismissTimer_ = new QTimer(this);
  trayDismissTimer_->setInterval(120);
  connect(trayDismissTimer_, &QTimer::timeout, this, [this] {
    if (!trayPopup_ || !isVisible()) {
      trayDismissTimer_->stop();
      return;
    }
#ifdef _WIN32
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return;
    DWORD foregroundProcess = 0;
    GetWindowThreadProcessId(foreground, &foregroundProcess);
    // Qt menus and modal dialogs have their own HWND but remain in this
    // process. Desktop/Explorer and other applications have a different PID.
    if (foregroundProcess != GetCurrentProcessId()) hide();
#endif
  });
  setAcceptDrops(true);

  auto* central = new QWidget(this);
  auto* layout = new QVBoxLayout(central);
  layout->setContentsMargins(18, 18, 18, 14);
  layout->setSpacing(12);
  central->setStyleSheet(QStringLiteral(
      "QPushButton { min-height: 34px; padding: 0 18px; border: 1px solid #c8d0d9; "
      "border-radius: 6px; background: #ffffff; color: #1f2937; }"
      "QPushButton:hover { background: #f3f6f9; }"
      "QPushButton#pairButton { background: #1769e0; border-color: #1769e0; color: white; }"
      "QPushButton#pairButton:hover { background: #125bc4; }"
      "QPushButton#deleteButton { color: #b42318; border-color: #e1b4b0; }"
      "QPushButton#deleteButton:hover { background: #fff1f0; }"
      "QPushButton#topButton:checked { background: #fff7dc; border-color: #d8a800; color: #6b5200; }"));
  auto* dropZone = new QFrame(central);
  dropZone_ = dropZone;
  dropZone->setObjectName(QStringLiteral("dropZone"));
  dropZone->setMinimumHeight(260);
  dropZone->setStyleSheet(QStringLiteral(
      "QFrame#dropZone { border: 2px dashed #73808c; border-radius: 8px; }"
      "QListWidget { border: none; background: transparent; outline: none; }"
      "QListWidget::indicator { width: 0px; height: 0px; }"
      "QListWidget::item { padding: 5px 8px; border: 1px solid transparent; border-radius: 6px; }"
      "QListWidget::item:hover { background: #edf5ff; border-color: #a9c9ef; color: #111827; }"
      "QListWidget::item:selected { background: #dcecff; border-color: #7eb0e8; color: #111827; }"
      "QListWidget::item:selected:hover { background: #cfe6ff; border-color: #4e91dc; color: #111827; }"));
  auto* dropLayout = new QVBoxLayout(dropZone);
  dropLayout->setContentsMargins(10, 10, 10, 10);
  auto* dropHint = new QLabel(
      QStringLiteral("拖入文件或文件夹即可上传 · 单击准备后直接拖出 · 双击另存为 · 右击管理"),
      dropZone);
  dropHint_ = dropHint;
  dropHint->setAlignment(Qt::AlignCenter);
  dropHint->setMinimumHeight(54);
  auto* nativeFiles = new NativeDragListWidget(dropZone);
  files_ = nativeFiles;
  files_->setMouseTracking(true);
  files_->viewport()->setAttribute(Qt::WA_Hover, true);
  files_->setAcceptDrops(false);
  files_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  files_->setDragEnabled(true);
  files_->setDragDropMode(QAbstractItemView::DragOnly);
  files_->setDefaultDropAction(Qt::CopyAction);
  files_->setViewMode(QListView::IconMode);
  files_->setIconSize(QSize(48, 48));
  files_->setGridSize(QSize(124, 94));
  files_->setResizeMode(QListView::Adjust);
  files_->setMovement(QListView::Static);
  files_->setWordWrap(true);
  files_->setTextElideMode(Qt::ElideMiddle);
  files_->setSpacing(4);
  dropLayout->addWidget(dropHint);
  dropLayout->addWidget(files_, 1);
  auto* delayedFile = new QPushButton(QStringLiteral("拖出 3 秒延迟虚拟文件（按住后拖到 Explorer）"), central);
  auto* delayedFolder = new QPushButton(QStringLiteral("拖出 15 秒延迟虚拟文件夹树（按住后拖到 Explorer）"), central);
  auto* pairButton = new QPushButton(QStringLiteral("设备设置 / 配对"), central);
  pairButton->setObjectName(QStringLiteral("pairButton"));
  auto* saveButton = new QPushButton(QStringLiteral("另存选中项（文件夹会自动安全解压）"), central);
  auto* dragRemoteButton = new QPushButton(QStringLiteral("拖出选中云端项（按住并拖到 Explorer）"), central);
  auto* deleteButton = new QPushButton(QStringLiteral("删除选中项"), central);
  deleteButton->setObjectName(QStringLiteral("deleteButton"));
  auto* topButton = new QPushButton(QStringLiteral("置顶显示"), central);
  topButton->setObjectName(QStringLiteral("topButton"));
  topButton->setCheckable(true);
  layout->addWidget(dropZone, 1);
  actionBar_ = new QWidget(central);
  auto* actions = new QHBoxLayout(actionBar_);
  actions->setContentsMargins(0, 0, 0, 0);
  actions->setSpacing(10);
  actions->addStretch(1);
  actions->addWidget(topButton);
  actions->addWidget(deleteButton);
  actions->addWidget(pairButton);
  layout->addWidget(actionBar_);
  delayedFile->hide();
  delayedFolder->hide();
  saveButton->hide();
  dragRemoteButton->hide();
  setCentralWidget(central);

  connect(topButton, &QPushButton::toggled, this, [this, topButton](bool enabled) {
    const QPoint position = pos();
    setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
    move(position);
    show();
    if (enabled) raise();
    topButton->setText(enabled ? QStringLiteral("取消置顶") : QStringLiteral("置顶显示"));
  });

#ifdef _WIN32
  connect(delayedFile, &QPushButton::pressed, this, [this] {
    const std::string text = "DropSpace delayed virtual file\r\n";
    windows::VirtualFileEntry entry{
        L"DropSpace-delay-3s.txt", false, text.size(),
        [text] {
          std::this_thread::sleep_for(std::chrono::seconds(3));
          std::vector<std::byte> result(text.size());
          std::memcpy(result.data(), text.data(), text.size());
          return result;
        }};
    std::wstring error;
    if (!windows::startVirtualFileDrag({std::move(entry)}, &error) && !error.empty()) {
      QMessageBox::warning(this, QStringLiteral("OLE 拖出失败"), QString::fromStdWString(error));
    }
  });
  connect(delayedFolder, &QPushButton::pressed, this, [this] {
    const std::string nested = "Unicode folder payload\r\n";
    std::vector<windows::VirtualFileEntry> entries;
    entries.push_back({L"DropSpace 文件夹", true, 0, {}}); 
    entries.push_back({L"DropSpace 文件夹\\空目录", true, 0, {}});
    entries.push_back({L"DropSpace 文件夹\\嵌套", true, 0, {}});
    entries.push_back({L"DropSpace 文件夹\\嵌套\\内容.txt", false, nested.size(),
                       [nested] {
                         std::this_thread::sleep_for(std::chrono::seconds(15));
                         std::vector<std::byte> result(nested.size());
                         std::memcpy(result.data(), nested.data(), nested.size());
                         return result;
                       }});
    std::wstring error;
    if (!windows::startVirtualFileDrag(std::move(entries), &error) && !error.empty()) {
      QMessageBox::warning(this, QStringLiteral("OLE 拖出失败"), QString::fromStdWString(error));
    }
  });
#else
  delayedFile->setEnabled(false);
  delayedFolder->setEnabled(false);
#endif

  status_ = new QLabel(QStringLiteral("自动缓存 0 / 52,428,800 bytes · 临时传输占用 0"), this);
  statusBar()->addPermanentWidget(status_);

  database_ = std::make_unique<ClientDatabase>();
  QString databaseError;
  if (!database_->open(&databaseError)) {
    QMessageBox::critical(this, QStringLiteral("本地数据库失败"), databaseError);
  } else {
    api_ = new ApiClient(database_.get(), this);
    connect(api_, &ApiClient::statusChanged, status_, &QLabel::setText);
    connect(api_, &ApiClient::failed, this, [this](const QString& message) {
      status_->setText(message);
      QMessageBox::warning(this, QStringLiteral("DropSpace"), message);
    });
    connect(api_, &ApiClient::snapshotReady, this, [this, dragRemoteButton](const QJsonArray& files) {
      files_->clear();
      QFileIconProvider iconProvider;
      for (const auto& value : files) {
        const auto file = value.toObject();
        const bool folder = file.value(QStringLiteral("kind")).toString() == QStringLiteral("folder");
        const QString type = folder
                                 ? QStringLiteral("文件夹") : QStringLiteral("文件");
        const bool own = file.value(QStringLiteral("upload_device_id")).toString() == database_->identity(nullptr).deviceId;
        const QString name = file.value(QStringLiteral("name")).toString();
        const QIcon icon = folder ? iconProvider.icon(QFileIconProvider::Folder)
                                  : iconProvider.icon(QFileInfo(name));
        auto* item = new QListWidgetItem(icon, name, files_);
        item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
        item->setData(Qt::CheckStateRole, QVariant());
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        item->setToolTip(QStringLiteral("%1\n类型：%2\n对象大小：%3 bytes\n状态：%4")
                             .arg(name, type)
                             .arg(file.value(QStringLiteral("object_size_bytes")).toInteger())
                             .arg(own ? QStringLiteral("本机上传") : QStringLiteral("云端/自动缓存")));
        item->setData(Qt::UserRole, file.toVariantMap());
        if (folder) {
          api_->fetchFolderManifest(file, [this, id = file.value(QStringLiteral("id")).toString()]
              (const QJsonArray& entries, const QString& error) {
            if (error.isEmpty()) folderManifests_.insert(id, entries);
          });
        }
      }
#ifdef __APPLE__
      appendP1PromiseRows(files_);
#endif
      applyView();
      applySort();
    });
    connect(api_, &ApiClient::cacheUsageChanged, this, [this](qint64 bytes) {
      status_->setText(QStringLiteral("自动缓存 %1 / 52,428,800 bytes").arg(bytes));
    });
#ifdef __APPLE__
    if (!p1PromiseMode()) {
#endif
    QString restoreError;
    if (!api_->restore(&restoreError) && !restoreError.isEmpty()) {
      status_->setText(QStringLiteral("凭据恢复失败：%1").arg(restoreError));
    }
#ifdef __APPLE__
    }
#endif
  }

  connect(pairButton, &QPushButton::clicked, this, [this] {
    if (!api_) return;
    bool ok = false;
    const QString server = QInputDialog::getText(this, QStringLiteral("DropSpace 服务器"),
        QStringLiteral("HTTPS 地址"), QLineEdit::Normal,
        QStringLiteral("https://43.108.80.211:8443"), &ok);
    if (!ok || server.isEmpty()) return;
    const QString token = QInputDialog::getText(this, QStringLiteral("设备配对"),
        QStringLiteral("一次性配对令牌"), QLineEdit::Password, {}, &ok);
    if (!ok || token.isEmpty()) return;
    const QString name = QInputDialog::getText(this, QStringLiteral("设备名称"),
        QStringLiteral("此设备显示名称"), QLineEdit::Normal,
        QSysInfo::machineHostName(), &ok);
    if (ok && !name.trimmed().isEmpty()) api_->pair(server, token, name);
  });

  auto saveSelected = [this] {
    if (!api_) return;
    QList<QJsonObject> selectedFiles;
    for (auto* item : files_->selectedItems()) {
      const auto file = QJsonObject::fromVariantMap(item->data(Qt::UserRole).toMap());
      if (!file.isEmpty()) selectedFiles.push_back(file);
    }
    if (selectedFiles.isEmpty()) return;
    if (selectedFiles.size() > 1) {
      const QString parent = QFileDialog::getExistingDirectory(this, QStringLiteral("选择多个项目的交付目录"));
      if (parent.isEmpty()) return;
      QSet<QString> reserved;
      for (const auto& selected : selectedFiles) {
        const bool selectedFolder = selected.value(QStringLiteral("kind")).toString() == QStringLiteral("folder");
        QString original = selected.value(QStringLiteral("name")).toString();
        QString candidate = original; int suffix = 2;
        const QFileInfo info(original);
        const QString stem = selectedFolder || info.suffix().isEmpty() ? original : info.completeBaseName();
        const QString extension = selectedFolder || info.suffix().isEmpty() ? QString() : u'.' + info.suffix();
        while (reserved.contains(candidate.toCaseFolded()) || QFileInfo::exists(QDir(parent).filePath(candidate)))
          candidate = QStringLiteral("%1 (%2)%3").arg(stem).arg(suffix++).arg(extension);
        reserved.insert(candidate.toCaseFolded());
        api_->exportObject(selected, QDir(parent).filePath(candidate));
      }
      return;
    }
    const auto file = selectedFiles.first();
    const bool folder = file.value(QStringLiteral("kind")).toString() == QStringLiteral("folder");
    QString destination;
    if (folder) {
      const QString parent = QFileDialog::getExistingDirectory(this, QStringLiteral("选择交付到哪个目录"));
      if (!parent.isEmpty()) destination = QDir(parent).filePath(file.value(QStringLiteral("name")).toString());
    } else {
      destination = QFileDialog::getSaveFileName(this, QStringLiteral("另存文件"),
                                                  file.value(QStringLiteral("name")).toString());
    }
    if (!destination.isEmpty()) api_->exportObject(file, destination);
  };
  connect(saveButton, &QPushButton::clicked, this, saveSelected);
  connect(files_, &QListWidget::itemDoubleClicked, this, [saveSelected](QListWidgetItem*) { saveSelected(); });
  auto deleteSelected = [this] {
    if (!api_) return;
    QList<QJsonObject> selectedFiles;
    QStringList names;
    for (auto* item : files_->selectedItems()) {
      const auto file = QJsonObject::fromVariantMap(item->data(Qt::UserRole).toMap());
      if (!file.isEmpty()) { selectedFiles.push_back(file); names.push_back(file.value(QStringLiteral("name")).toString()); }
    }
    if (selectedFiles.isEmpty()) return;
    const QString target = selectedFiles.size() == 1
        ? QStringLiteral("“%1”").arg(names.first())
        : QStringLiteral("选中的 %1 个条目").arg(selectedFiles.size());
    if (QMessageBox::question(this, QStringLiteral("删除云端条目"),
          QStringLiteral("确定从所有设备删除%1吗？本地原文件不会删除。")
              .arg(target)) == QMessageBox::Yes) {
      for (const auto& file : selectedFiles) api_->deleteObject(file);
    }
  };
  connect(deleteButton, &QPushButton::clicked, this, deleteSelected);
  files_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(files_, &QListWidget::customContextMenuRequested, this,
          [this, saveSelected, deleteSelected](const QPoint& position) {
    auto* item = files_->itemAt(position);
    QMenu menu(this);
    if (item && !item->data(Qt::UserRole).toMap().isEmpty()) {
      if (!item->isSelected()) { files_->clearSelection(); item->setSelected(true); }
      files_->setCurrentItem(item, QItemSelectionModel::NoUpdate);
      auto* save = menu.addAction(QStringLiteral("另存为…"));
      menu.addSeparator();
      auto* remove = menu.addAction(QStringLiteral("删除云端条目"));
      remove->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
      const QAction* selected = menu.exec(files_->viewport()->mapToGlobal(position));
      if (selected == save) saveSelected();
      else if (selected == remove) deleteSelected();
      return;
    }

    auto* refresh = menu.addAction(style()->standardIcon(QStyle::SP_BrowserReload),
                                   QStringLiteral("刷新"));
    menu.addSeparator();
    auto* sortMenu = menu.addMenu(QStringLiteral("排序方式"));
    QActionGroup sortGroup(sortMenu);
    sortGroup.setExclusive(true);
    auto addSort = [&](const QString& text, SortMode mode) {
      auto* action = sortMenu->addAction(text); action->setCheckable(true);
      action->setChecked(sortMode_ == mode); sortGroup.addAction(action); return action;
    };
    auto* byName = addSort(QStringLiteral("名称"), SortMode::Name);
    auto* bySize = addSort(QStringLiteral("大小"), SortMode::Size);
    auto* byType = addSort(QStringLiteral("项目类型"), SortMode::Type);
    auto* byTime = addSort(QStringLiteral("上传时间"), SortMode::UploadedAt);
    sortMenu->addSeparator();
    QActionGroup directionGroup(sortMenu);
    directionGroup.setExclusive(true);
    auto* ascending = sortMenu->addAction(QStringLiteral("升序"));
    auto* descending = sortMenu->addAction(QStringLiteral("降序"));
    ascending->setCheckable(true); descending->setCheckable(true);
    ascending->setChecked(sortAscending_); descending->setChecked(!sortAscending_);
    directionGroup.addAction(ascending); directionGroup.addAction(descending);

    auto* viewMenu = menu.addMenu(QStringLiteral("查看"));
    QActionGroup viewGroup(viewMenu); viewGroup.setExclusive(true);
    auto* icons = viewMenu->addAction(QStringLiteral("图标"));
    auto* list = viewMenu->addAction(QStringLiteral("列表"));
    icons->setCheckable(true); list->setCheckable(true);
    icons->setChecked(iconView_); list->setChecked(!iconView_);
    viewGroup.addAction(icons); viewGroup.addAction(list);

    const QAction* selected = menu.exec(files_->viewport()->mapToGlobal(position));
    if (selected == refresh) { if (api_) api_->refreshSnapshot(); return; }
    if (selected == byName) sortMode_ = SortMode::Name;
    else if (selected == bySize) sortMode_ = SortMode::Size;
    else if (selected == byType) sortMode_ = SortMode::Type;
    else if (selected == byTime) sortMode_ = SortMode::UploadedAt;
    else if (selected == ascending) sortAscending_ = true;
    else if (selected == descending) sortAscending_ = false;
    else if (selected == icons) { iconView_ = true; applyView(); return; }
    else if (selected == list) { iconView_ = false; applyView(); return; }
    else return;
    applySort();
  });

#ifdef _WIN32
  dragRemoteButton->setEnabled(false);
  connect(files_, &QListWidget::itemSelectionChanged, this, [this] {
    for (auto* item : files_->selectedItems()) {
      const auto file = QJsonObject::fromVariantMap(item->data(Qt::UserRole).toMap());
      if (!file.isEmpty()) prepareForDrag(file);
    }
  });

  auto startSelectedDrag = [this] {
    if (!api_) return;
    QList<QJsonObject> selectedFiles;
    for (auto* item : files_->selectedItems()) {
      const auto file = QJsonObject::fromVariantMap(item->data(Qt::UserRole).toMap());
      if (!file.isEmpty()) selectedFiles.push_back(file);
    }
    if (selectedFiles.isEmpty()) return;
    std::vector<windows::VirtualFileEntry> virtualEntries;
    QSet<QString> usedNames;
    auto uniqueName = [&usedNames](QString name, bool folder) {
      for (const QChar invalid : QStringLiteral("<>:\"/\\|?*")) name.replace(invalid, u'_');
      name = name.left(120).trimmed(); if (name.isEmpty()) name = QStringLiteral("DropSpace item");
      QString candidate = name; int suffix = 2;
      const QFileInfo info(name);
      const QString stem = folder || info.suffix().isEmpty() ? name : info.completeBaseName();
      const QString extension = folder || info.suffix().isEmpty() ? QString() : u'.' + info.suffix();
      while (usedNames.contains(candidate.normalized(QString::NormalizationForm_C).toCaseFolded()))
        candidate = QStringLiteral("%1 (%2)%3").arg(stem).arg(suffix++).arg(extension);
      usedNames.insert(candidate.normalized(QString::NormalizationForm_C).toCaseFolded());
      return candidate;
    };
    for (const auto& file : selectedFiles) {
      const QString id = file.value(QStringLiteral("id")).toString();
      const bool folder = file.value(QStringLiteral("kind")).toString() == QStringLiteral("folder");
      const QString name = uniqueName(file.value(QStringLiteral("name")).toString(), folder);
      if (!folder) {
        const QString preparedPath = preparedDragObjects_.value(id);
        if (preparedPath.isEmpty()) { status_->setText(QStringLiteral("选中的文件尚未全部准备完成")); return; }
        windows::VirtualFileEntry entry;
        entry.relativePath = name.toStdWString();
        entry.size = file.value(QStringLiteral("object_size_bytes")).toInteger();
        entry.provideFilePath = [preparedPath] { return preparedPath.toStdWString(); };
        virtualEntries.push_back(std::move(entry));
        continue;
      }
      const QString preparedRoot = preparedDragFolders_.value(id);
      if (!folderManifests_.contains(id) || preparedRoot.isEmpty()) {
        status_->setText(QStringLiteral("选中的文件夹尚未全部准备完成")); return;
      }
      virtualEntries.push_back({name.toStdWString(), true, 0, {}, {}});
      for (const auto& value : folderManifests_.value(id)) {
        const auto member = value.toObject(); const QString relative = member.value(QStringLiteral("path")).toString();
        const QString virtualPath = name + u'\\' + QString(relative).replace(u'/', u'\\');
        if (virtualPath.size() >= 260) { status_->setText(QStringLiteral("文件夹含超过 OLE 限制的长路径")); return; }
        windows::VirtualFileEntry entry;
        entry.relativePath = virtualPath.toStdWString(); entry.directory = member.value(QStringLiteral("directory")).toBool();
        entry.size = member.value(QStringLiteral("size")).toInteger();
        if (!entry.directory) entry.provideFilePath = [preparedRoot, relative] {
          return QDir(preparedRoot).filePath(relative).toStdWString();
        };
        virtualEntries.push_back(std::move(entry));
      }
    }
    std::wstring error;
    if (!windows::startVirtualFileDrag(std::move(virtualEntries), &error) && !error.empty())
      QMessageBox::warning(this, QStringLiteral("云端拖出失败"), QString::fromStdWString(error));
  };
  nativeFiles->startNativeDrag = startSelectedDrag;
  connect(dragRemoteButton, &QPushButton::pressed, this, startSelectedDrag);
#elif defined(__APPLE__)
  auto startSelectedDrag = [this] {
    const auto selected = files_->selectedItems();
    if (selected.isEmpty()) return;
    std::vector<macos::FilePromiseEntry> promises;
    promises.reserve(static_cast<std::size_t>(selected.size()));
    for (auto* item : selected) {
      const auto file = QJsonObject::fromVariantMap(item->data(Qt::UserRole).toMap());
      if (file.isEmpty()) continue;
      const QString demo = file.value(QStringLiteral("p1_demo")).toString();
      const bool folder = file.value(QStringLiteral("kind")).toString() == QStringLiteral("folder");
      macos::FilePromiseEntry entry;
      entry.fileName = file.value(QStringLiteral("name")).toString();
      entry.folder = folder;
      entry.contentLease = std::make_shared<int>(1);
      if (!demo.isEmpty()) {
        const QString sessionRoot = QDir(QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation)).filePath(
                QStringLiteral("transfers/p1-promises/%1")
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QDir().mkpath(sessionRoot);
        entry.contentLease = std::shared_ptr<void>(
            new QString(sessionRoot), [](void* value) {
              std::unique_ptr<QString> root(static_cast<QString*>(value));
              QDir(*root).removeRecursively();
            });
        entry.acquireObject = [sessionRoot, folder](QString* error) {
          std::this_thread::sleep_for(folder ? std::chrono::seconds(15)
                                             : std::chrono::seconds(3));
          if (!folder) {
            const QString object = QDir(sessionRoot).filePath(QStringLiteral("delayed-object.bin"));
            QFile output(object);
            if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                output.write("DropSpace delayed Finder file promise\n") < 0) {
              *error = QStringLiteral("无法生成 P1 延迟文件对象");
              return QString();
            }
            output.close();
            return object;
          }
          const QString source = QDir(sessionRoot).filePath(QStringLiteral("source"));
          const QString nested = QDir(source).filePath(QStringLiteral("嵌套/Unicode 名称"));
          const QString empty = QDir(source).filePath(QStringLiteral("空目录"));
          if (!QDir().mkpath(nested) || !QDir().mkpath(empty)) {
            *error = QStringLiteral("无法生成 P1 文件夹源树");
            return QString();
          }
          QFile payload(QDir(nested).filePath(QStringLiteral("内容.txt")));
          if (!payload.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
              payload.write("DropSpace Unicode folder promise\n") < 0) {
            *error = QStringLiteral("无法生成 P1 文件夹内容");
            return QString();
          }
          payload.close();
          const QString zip = QDir(sessionRoot).filePath(QStringLiteral("folder.zip"));
          PackedFolder packed;
          if (!packFolder(source, zip, &packed, error)) return QString();
          QDir(source).removeRecursively();
          return zip;
        };
      } else {
        if (!api_) continue;
        entry.acquireObject = [api = api_, file](QString* error) {
          return api->acquireReadableObjectBlocking(file, error);
        };
      }
      promises.push_back(std::move(entry));
    }
    QString error;
    if (!promises.empty() && !macos::startFilePromiseDrag(files_->viewport(),
                                                           std::move(promises), &error)) {
      status_->setText(QStringLiteral("Finder File Promise 启动失败：%1").arg(error));
    }
  };
  nativeFiles->startNativeDrag = startSelectedDrag;
#else
  dragRemoteButton->setEnabled(false);
#endif

#ifdef __APPLE__
  appendP1PromiseRows(files_);
  if (p1PromiseMode())
    status_->setText(QStringLiteral("P1 Finder Promise 模式：拖动两个测试条目到 Finder"));
#endif
}

MainWindow::~MainWindow() = default;

void MainWindow::showFullWindow() {
  hide();
  trayDismissTimer_->stop();
  trayPopup_ = false;
  setWindowFlags(Qt::Window);
  actionBar_->show();
  dropHint_->show();
  dropZone_->setMinimumHeight(260);
  statusBar()->show();
  if (normalGeometry_.isValid()) setGeometry(normalGeometry_);
  showNormal();
  raise();
  activateWindow();
}

void MainWindow::showTrayPopup(const QRect& trayGeometry) {
  if (!trayPopup_ && isVisible()) normalGeometry_ = geometry();
  hide();
  trayPopup_ = true;
  actionBar_->hide();
  dropHint_->hide();
  dropZone_->setMinimumHeight(0);
  statusBar()->hide();
  // Qt::Popup is dismissed by Windows as soon as the context QMenu takes
  // activation, which makes right-click actions inside the tray surface
  // unreliable. A frameless tool window stays out of the taskbar while the
  // WindowDeactivate handler below preserves click-outside dismissal.
  setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
  // Keep the left-click surface compact: only the dashed file area with two
  // icon rows. Full-window and quit actions belong to the tray right-click
  // menu and are never embedded in this popup.
  resize(520, 240);
  const QScreen* screen = trayGeometry.isValid()
      ? QGuiApplication::screenAt(trayGeometry.center())
      : QGuiApplication::primaryScreen();
  const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
  int x = trayGeometry.isValid() ? trayGeometry.center().x() - width() / 2
                                 : available.right() - width();
  int y = trayGeometry.isValid() ? trayGeometry.top() - height() - 8
                                 : available.bottom() - height();
  x = std::clamp(x, available.left(), available.right() - width() + 1);
  y = std::clamp(y, available.top(), available.bottom() - height() + 1);
  move(x, y);
  show();
  raise();
  activateWindow();
  trayDismissTimer_->start();
}

void MainWindow::changeEvent(QEvent* event) {
  QMainWindow::changeEvent(event);
  if (!trayPopup_ || event->type() != QEvent::WindowDeactivate) return;

  // Defer the decision until Qt has activated a context menu or modal dialog.
  // Those child surfaces must remain usable from the tray popup. A genuine
  // click into another application leaves neither active and hides the popup.
  QTimer::singleShot(0, this, [this] {
    if (!trayPopup_ || !isVisible()) return;
    if (QApplication::activePopupWidget() || QApplication::activeModalWidget()) return;
    if (QApplication::activeWindow() == this) return;
    hide();
  });
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (!trayPopup_) normalGeometry_ = geometry();
  trayDismissTimer_->stop();
  hide();
  event->ignore();
}

void MainWindow::prepareForDrag(const QJsonObject& file) {
  if (!api_) return;
  const QString id = file.value(QStringLiteral("id")).toString();
  if (id.isEmpty()) return;
  const bool folder = file.value(QStringLiteral("kind")).toString() == QStringLiteral("folder");
  const QString name = file.value(QStringLiteral("name")).toString();
  if (folder && !folderManifests_.contains(id)) {
    api_->fetchFolderManifest(file, [this, id, name](const QJsonArray& entries, const QString& error) {
      if (!error.isEmpty()) status_->setText(QStringLiteral("%1 的文件夹清单失败：%2").arg(name, error));
      else { folderManifests_.insert(id, entries); updatePreparedSelectionStatus(); }
    });
  }
  if ((!folder && preparedDragObjects_.contains(id)) ||
      (folder && preparedDragFolders_.contains(id)) || preparingDragIds_.contains(id)) return;
  preparingDragIds_.insert(id);
  status_->setText(QStringLiteral("正在准备 %1…").arg(name));
  api_->acquireReadableObject(file, [this, id, name, folder](const QString& objectPath,
                                                              const QString& error) {
    if (!error.isEmpty()) {
      preparingDragIds_.remove(id);
      status_->setText(QStringLiteral("%1 准备失败：%2").arg(name, error));
      return;
    }
    if (!folder) {
      preparingDragIds_.remove(id);
      preparedDragObjects_.insert(id, objectPath);
      status_->setText(QStringLiteral("%1 已准备好，可以拖出").arg(name));
      updatePreparedSelectionStatus();
      return;
    }
    const QString base = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("transfers/exports"));
    QDir().mkpath(base);
    const QString destination = QDir(base).filePath(
        QStringLiteral("drag-%1-%2").arg(id, QUuid::createUuid().toString(QUuid::WithoutBraces)));
    auto* watcher = new QFutureWatcher<QPair<QString, QString>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, id, name] {
      const auto result = watcher->result(); watcher->deleteLater(); preparingDragIds_.remove(id);
      if (!result.second.isEmpty()) {
        status_->setText(QStringLiteral("%1 准备失败：%2").arg(name, result.second));
        return;
      }
      preparedDragFolders_.insert(id, result.first);
      status_->setText(QStringLiteral("%1 已安全准备好，可以拖出").arg(name));
      updatePreparedSelectionStatus();
    });
    watcher->setFuture(QtConcurrent::run([objectPath, destination] {
      std::vector<ExtractedEntry> extracted; QString extractionError;
      if (!safeExtractZip(objectPath, destination, &extracted, &extractionError))
        return QPair<QString, QString>{QString(), extractionError};
      return QPair<QString, QString>{destination, QString()};
    }));
  });
}

void MainWindow::updatePreparedSelectionStatus() {
  const auto selected = files_->selectedItems();
  if (selected.isEmpty()) return;
  for (auto* item : selected) {
    const auto file = QJsonObject::fromVariantMap(item->data(Qt::UserRole).toMap());
    if (file.isEmpty()) continue;
    const QString id = file.value(QStringLiteral("id")).toString();
    const bool folder = file.value(QStringLiteral("kind")).toString() == QStringLiteral("folder");
    if ((!folder && !preparedDragObjects_.contains(id)) ||
        (folder && (!preparedDragFolders_.contains(id) || !folderManifests_.contains(id)))) return;
  }
  status_->setText(selected.size() == 1
      ? QStringLiteral("选中项目已准备好，可以直接拖出")
      : QStringLiteral("选中的 %1 个项目已全部准备好，可以一起拖出").arg(selected.size()));
}

void MainWindow::applySort() {
  std::vector<QListWidgetItem*> items;
  items.reserve(static_cast<std::size_t>(files_->count()));
  while (files_->count()) items.push_back(files_->takeItem(0));
  QCollator collator;
  collator.setCaseSensitivity(Qt::CaseInsensitive);
  collator.setNumericMode(true);
  auto metadata = [](const QListWidgetItem* item) {
    return QJsonObject::fromVariantMap(item->data(Qt::UserRole).toMap());
  };
  auto keyText = [this](const QJsonObject& file) {
    switch (sortMode_) {
      case SortMode::Name:
        return file.value(QStringLiteral("name")).toString();
      case SortMode::Type:
        if (file.value(QStringLiteral("kind")).toString() == QStringLiteral("folder"))
          return QStringLiteral("0|文件夹");
        return QStringLiteral("1|%1").arg(
            QFileInfo(file.value(QStringLiteral("name")).toString()).suffix().toCaseFolded());
      case SortMode::UploadedAt:
        return file.value(QStringLiteral("created_at")).toString();
      case SortMode::Size:
        return QString();
    }
    return QString();
  };
  std::stable_sort(items.begin(), items.end(), [&](const auto* left, const auto* right) {
    const auto a = metadata(left), b = metadata(right);
    if (a.isEmpty() != b.isEmpty()) return !a.isEmpty();
    int comparison = 0;
    if (sortMode_ == SortMode::Size) {
      const qint64 av = a.value(QStringLiteral("object_size_bytes")).toInteger();
      const qint64 bv = b.value(QStringLiteral("object_size_bytes")).toInteger();
      comparison = av < bv ? -1 : (av > bv ? 1 : 0);
    } else {
      comparison = collator.compare(keyText(a), keyText(b));
    }
    if (comparison == 0)
      comparison = collator.compare(a.value(QStringLiteral("name")).toString(),
                                    b.value(QStringLiteral("name")).toString());
    return sortAscending_ ? comparison < 0 : comparison > 0;
  });
  for (auto* item : items) files_->addItem(item);
}

void MainWindow::applyView() {
  if (iconView_) {
    files_->setViewMode(QListView::IconMode);
    files_->setIconSize(QSize(48, 48));
    files_->setGridSize(QSize(124, 94));
    files_->setWordWrap(true);
  } else {
    files_->setViewMode(QListView::ListMode);
    files_->setIconSize(QSize(24, 24));
    files_->setGridSize(QSize());
    files_->setWordWrap(false);
  }
  for (int index = 0; index < files_->count(); ++index) {
    auto* item = files_->item(index);
    const auto file = QJsonObject::fromVariantMap(item->data(Qt::UserRole).toMap());
    if (file.isEmpty()) continue;
    const QString name = file.value(QStringLiteral("name")).toString();
    if (iconView_) {
      item->setText(name);
      item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
    } else {
      const bool folder = file.value(QStringLiteral("kind")).toString() == QStringLiteral("folder");
      const QString type = folder ? QStringLiteral("文件夹")
                                  : (QFileInfo(name).suffix().isEmpty()
                                         ? QStringLiteral("文件")
                                         : QFileInfo(name).suffix().toUpper() + QStringLiteral(" 文件"));
      const QDateTime uploaded = QDateTime::fromString(
          file.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs).toLocalTime();
      item->setText(QStringLiteral("%1    %2    %3 bytes    %4")
                        .arg(name, type)
                        .arg(file.value(QStringLiteral("object_size_bytes")).toInteger())
                        .arg(uploaded.isValid() ? uploaded.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                                                : QStringLiteral("—")));
      item->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    }
  }
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
  if (event->mimeData()->hasUrls()) {
    const auto urls = event->mimeData()->urls();
    const bool allLocal = std::all_of(urls.begin(), urls.end(),
                                      [](const QUrl& url) { return url.isLocalFile(); });
    if (allLocal) event->acceptProposedAction();
  }
}

void MainWindow::dropEvent(QDropEvent* event) {
  QFileIconProvider iconProvider;
  for (const auto& url : event->mimeData()->urls()) {
    const QFileInfo info(url.toLocalFile());
    if (info.exists()) {
      const QIcon icon = info.isDir() ? iconProvider.icon(QFileIconProvider::Folder)
                                      : iconProvider.icon(info);
      auto* item = new QListWidgetItem(icon, info.fileName(), files_);
      item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
      item->setData(Qt::CheckStateRole, QVariant());
      item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
      item->setToolTip(QStringLiteral("%1\n正在进入上传队列…").arg(info.fileName()));
      if (api_) api_->uploadPath(info.absoluteFilePath());
    }
  }
  event->acceptProposedAction();
}

}  // namespace dropspace

