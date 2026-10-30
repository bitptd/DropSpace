#include "ui/ClipboardHistoryDialog.h"

#include "core/ClipboardController.h"

#include <QDateTime>
#include <QIcon>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPixmap>
#include <QStyle>
#include <QVBoxLayout>

namespace dropspace {

ClipboardHistoryDialog::ClipboardHistoryDialog(ClipboardController* controller, QWidget* parent)
    : QDialog(parent), controller_(controller) {
  setWindowTitle(QStringLiteral("共享剪贴板历史"));
  setWindowFlag(Qt::Tool, true);
  resize(460, 520);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(12, 12, 12, 12);
  auto* hint = new QLabel(QStringLiteral("最近 20 条 · ↑↓ 选择 · Enter 写入剪贴板 · Esc 关闭"), this);
  hint->setStyleSheet(QStringLiteral("color:#667085; padding:2px 4px 8px 4px;"));
  layout->addWidget(hint);
  items_ = new QListWidget(this);
  items_->setIconSize(QSize(58, 58));
  items_->setSpacing(3);
  items_->setWordWrap(true);
  items_->setTextElideMode(Qt::ElideRight);
  items_->setStyleSheet(QStringLiteral(
      "QListWidget { border:1px solid #d0d5dd; border-radius:8px; background:#fff; outline:none; }"
      "QListWidget::item { min-height:64px; padding:7px; border-bottom:1px solid #eaecf0; }"
      "QListWidget::item:hover { background:#f2f7ff; }"
      "QListWidget::item:selected { background:#dcecff; color:#101828; }"));
  items_->setContextMenuPolicy(Qt::CustomContextMenu);
  layout->addWidget(items_, 1);
  empty_ = new QLabel(QStringLiteral("暂无共享剪贴板历史"), items_);
  empty_->setAlignment(Qt::AlignCenter);
  empty_->setStyleSheet(QStringLiteral("color:#98a2b3; background:transparent;"));
  empty_->hide();

  connect(controller_, &ClipboardController::historyReady, this,
          [this](const QJsonArray& entries) { populate(entries); });
  connect(items_, &QListWidget::itemActivated, this,
          [this](QListWidgetItem*) { applyCurrent(); });
  connect(items_, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem*) { applyCurrent(); });
  connect(items_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& position) {
    auto* item = items_->itemAt(position);
    if (!item) return;
    items_->setCurrentItem(item);
    QMenu menu(this);
    auto* use = menu.addAction(QStringLiteral("写入系统剪贴板"));
    auto* remove = menu.addAction(QStringLiteral("删除此条历史"));
    const QAction* chosen = menu.exec(items_->viewport()->mapToGlobal(position));
    if (chosen == use) applyCurrent();
    if (chosen == remove)
      controller_->deleteHistoryItem(QJsonObject::fromVariantMap(item->data(Qt::UserRole).toMap()));
  });
}

void ClipboardHistoryDialog::openHistory() {
  controller_->refreshHistory();
  showNormal();
  raise();
  activateWindow();
  items_->setFocus();
}

void ClipboardHistoryDialog::populate(const QJsonArray& entries) {
  items_->clear();
  for (const auto& value : entries) {
    const QJsonObject item = value.toObject();
    const bool image = item.value(QStringLiteral("content_type")).toString() ==
                       QStringLiteral("image/png");
    QString preview = image ? QStringLiteral("[图片]")
                            : item.value(QStringLiteral("preview")).toString();
    preview.replace(u'\r', u' ');
    preview.replace(u'\n', u' ');
    if (preview.size() > 160) preview = preview.left(160) + QStringLiteral("…");
    const QDateTime copied = QDateTime::fromString(
        item.value(QStringLiteral("copied_at")).toString(), Qt::ISODateWithMs).toLocalTime();
    const QString source = item.value(QStringLiteral("source_device_name")).toString();
    const QString details = QStringLiteral("%1\n%2 · %3 · %4")
                                .arg(preview.isEmpty() ? QStringLiteral("[空文本]") : preview,
                                     source.isEmpty() ? QStringLiteral("未知设备") : source,
                                     copied.isValid() ? copied.toString(QStringLiteral("MM-dd HH:mm:ss"))
                                                      : QStringLiteral("未知时间"),
                                     image ? QStringLiteral("PNG 图片") : QStringLiteral("文本"));
    auto* row = new QListWidgetItem(details, items_);
    row->setData(Qt::UserRole, item.toVariantMap());
    row->setToolTip(preview);
    if (image) {
      row->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
      controller_->downloadThumbnail(item, [this, id = item.value(QStringLiteral("clipboard_id")).toString()]
          (const QByteArray& bytes, const QString& error) {
        if (!error.isEmpty()) return;
        QPixmap thumbnail;
        if (!thumbnail.loadFromData(bytes, "PNG")) return;
        for (int index = 0; index < items_->count(); ++index) {
          auto* candidate = items_->item(index);
          const auto metadata = QJsonObject::fromVariantMap(candidate->data(Qt::UserRole).toMap());
          if (metadata.value(QStringLiteral("clipboard_id")).toString() == id) {
            candidate->setIcon(QIcon(thumbnail));
            break;
          }
        }
      });
    } else {
      row->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    }
  }
  empty_->setVisible(entries.isEmpty());
  empty_->setGeometry(items_->rect());
  if (items_->count() > 0) items_->setCurrentRow(0);
}

void ClipboardHistoryDialog::applyCurrent() {
  auto* item = items_->currentItem();
  if (!item) return;
  if (!controller_->enabled()) {
    controller_->applyHistoryItem({});
    return;
  }
  controller_->applyHistoryItem(QJsonObject::fromVariantMap(item->data(Qt::UserRole).toMap()));
  accept();
}

void ClipboardHistoryDialog::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape) {
    reject();
    return;
  }
  if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
    applyCurrent();
    return;
  }
  QDialog::keyPressEvent(event);
}

}  // namespace dropspace
