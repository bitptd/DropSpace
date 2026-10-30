#pragma once

#include <QDialog>
#include <QJsonArray>

class QLabel;
class QListWidget;
class QKeyEvent;

namespace dropspace {

class ClipboardController;

class ClipboardHistoryDialog final : public QDialog {
 public:
  explicit ClipboardHistoryDialog(ClipboardController* controller,
                                  QWidget* parent = nullptr);
  void openHistory();

 protected:
  void keyPressEvent(QKeyEvent* event) override;

 private:
  void populate(const QJsonArray& items);
  void applyCurrent();

  ClipboardController* controller_;
  QListWidget* items_ = nullptr;
  QLabel* empty_ = nullptr;
};

}  // namespace dropspace
