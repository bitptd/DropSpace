#pragma once

#include <QString>

#include <functional>
#include <memory>
#include <vector>

class QWidget;

namespace dropspace::macos {

struct FilePromiseEntry {
  QString fileName;
  bool folder = false;
  QString fileType;
  std::function<QString(QString* error)> acquireObject;
  std::shared_ptr<void> contentLease;
};

// Install before the first mouse press so the AppKit event that Qt wraps in a
// QMouseEvent remains available when the native dragging session begins.
void installFilePromiseEventMonitor();
bool startFilePromiseDrag(QWidget* source, std::vector<FilePromiseEntry> entries,
                          QString* error);
int activeFilePromiseCount();

}  // namespace dropspace::macos
