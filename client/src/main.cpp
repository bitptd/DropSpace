#include "ui/MainWindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>
#include <QIcon>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QMessageBox>

#ifdef __APPLE__
#include "platform/macos/FilePromiseDrag.h"
#endif

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("DropSpace"));
  QCoreApplication::setApplicationName(QStringLiteral("DropSpace"));
  const QString externalIcon = QCoreApplication::applicationDirPath() + QStringLiteral("/icon.jpg");
  const QIcon applicationIcon(QFileInfo::exists(externalIcon)
                                  ? externalIcon
                                  : QStringLiteral(":/resources/icon.jpg"));
  app.setWindowIcon(applicationIcon);
  app.setQuitOnLastWindowClosed(false);
  dropspace::MainWindow window;

  QSystemTrayIcon tray(applicationIcon, &app);
  tray.setToolTip(QStringLiteral("DropSpace"));
  QMenu trayMenu;
  auto* showAction = trayMenu.addAction(QStringLiteral("打开完整窗口"));
  trayMenu.addSeparator();
  auto* quitAction = trayMenu.addAction(QStringLiteral("彻底退出"));
  tray.setContextMenu(&trayMenu);
  QObject::connect(showAction, &QAction::triggered, &app, [&window] {
    window.showFullWindow();
  });
  QObject::connect(&tray, &QSystemTrayIcon::activated, &app,
                   [&window, &tray](QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
      window.showTrayPopup(tray.geometry());
  });
  QObject::connect(quitAction, &QAction::triggered, &app, [&app, &tray] {
#ifdef __APPLE__
    if (dropspace::macos::activeFilePromiseCount() > 0 &&
        QMessageBox::question(nullptr, QStringLiteral("File Promise 正在进行"),
                              QStringLiteral("Finder 仍在接收内容。要取消并退出吗？")) !=
            QMessageBox::Yes)
      return;
#endif
    tray.hide();
    app.quit();
  });
  tray.show();
  window.show();
  return app.exec();
}

