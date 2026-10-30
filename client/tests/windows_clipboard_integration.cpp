#include "platform/windows/WindowsClipboard.h"

#include <QApplication>
#include <QBuffer>
#include <QEventLoop>
#include <QImage>
#include <QTimer>

#include <objidl.h>
#include <ole2.h>
#include <windows.h>

#include <cstring>
#include <iostream>

namespace {

bool setExternalText(const wchar_t* text) {
  const SIZE_T bytes = (wcslen(text) + 1) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
  if (!memory) return false;
  void* destination = GlobalLock(memory);
  if (!destination) {
    GlobalFree(memory);
    return false;
  }
  std::memcpy(destination, text, bytes);
  GlobalUnlock(memory);
  if (!OpenClipboard(nullptr)) {
    GlobalFree(memory);
    return false;
  }
  const bool success = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
  CloseClipboard();
  if (!success) GlobalFree(memory);
  return success;
}

void spin(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  const HRESULT ole = OleInitialize(nullptr);
  IDataObject* savedClipboard = nullptr;
  if (SUCCEEDED(ole)) OleGetClipboard(&savedClipboard);
  auto restore = [&] {
    if (savedClipboard) {
      OleSetClipboard(savedClipboard);
      savedClipboard->Release();
    }
    if (SUCCEEDED(ole)) OleUninitialize();
  };

  dropspace::windows::WindowsClipboard clipboard;
  QString error;
  if (!clipboard.initialize(&error) || !clipboard.setMonitoringEnabled(true, &error)) {
    std::cerr << error.toStdString() << '\n';
    restore();
    return 1;
  }
  int captures = 0;
  QString capturedType;
  QByteArray capturedBytes;
  QObject::connect(&clipboard, &dropspace::windows::WindowsClipboard::localContentCaptured,
                   [&](const QString& type, const QByteArray& bytes, const QDateTime&) {
    ++captures;
    capturedType = type;
    capturedBytes = bytes;
  });

  if (!setExternalText(L"DropSpace clipboard integration")) {
    std::cerr << "cannot seed Windows clipboard\n";
    restore();
    return 2;
  }
  spin(250);
  if (captures != 1 || capturedType != QStringLiteral("text") ||
      capturedBytes != QByteArray("DropSpace clipboard integration")) {
    std::cerr << "local text capture mismatch\n";
    restore();
    return 3;
  }

  if (!clipboard.writeRemote(QStringLiteral("clipboard-integration-0001"),
                             QString(64, u'a'), QStringLiteral("text"),
                             QByteArray("remote text"), &error)) {
    std::cerr << error.toStdString() << '\n';
    restore();
    return 4;
  }
  spin(250);
  if (captures != 1) {
    std::cerr << "DropSpace text write was captured again\n";
    restore();
    return 5;
  }

  QImage image(8, 6, QImage::Format_ARGB32);
  image.fill(QColor(23, 105, 224, 255));
  QByteArray png;
  QBuffer buffer(&png);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  if (!clipboard.writeRemote(QStringLiteral("clipboard-integration-0002"),
                             QString(64, u'b'), QStringLiteral("image/png"), png, &error)) {
    std::cerr << error.toStdString() << '\n';
    restore();
    return 6;
  }
  spin(250);
  if (captures != 1 || !IsClipboardFormatAvailable(RegisterClipboardFormatW(L"PNG")) ||
      !IsClipboardFormatAvailable(CF_DIBV5)) {
    std::cerr << "remote image formats or loop suppression mismatch\n";
    restore();
    return 7;
  }

  restore();
  std::cout << "Windows clipboard text, PNG, and loop-marker integration passed\n";
  return 0;
}
