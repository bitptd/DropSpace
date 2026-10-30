#include "platform/windows/WindowsClipboard.h"

#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QCursor>
#include <QImage>
#include <QImageReader>
#include <QTimer>

#include <algorithm>
#include <cstring>

namespace dropspace::windows {
namespace {

constexpr int kHistoryHotkeyId = 0x4453;
constexpr DWORD kBiAlphaBitfields = 6;
constexpr wchar_t kWindowClassName[] = L"DropSpaceClipboardMessageWindowV1";
constexpr wchar_t kMarkerFormatName[] = L"DropSpace.Clipboard.Source.v1";

QString windowsError(const QString& operation) {
  return QStringLiteral("%1（Windows 错误 %2）").arg(operation).arg(GetLastError());
}

QByteArray globalBytes(HGLOBAL handle) {
  if (!handle) return {};
  const SIZE_T size = GlobalSize(handle);
  if (size == 0 || size > static_cast<SIZE_T>(64 * 1024 * 1024)) return {};
  const void* locked = GlobalLock(handle);
  if (!locked) return {};
  QByteArray result(static_cast<const char*>(locked), static_cast<qsizetype>(size));
  GlobalUnlock(handle);
  return result;
}

bool setGlobalClipboardData(UINT format, const QByteArray& bytes, QString* error) {
  const SIZE_T allocationSize = std::max<SIZE_T>(1, static_cast<SIZE_T>(bytes.size()));
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, allocationSize);
  if (!memory) {
    if (error) *error = windowsError(QStringLiteral("无法分配剪贴板内存"));
    return false;
  }
  void* destination = GlobalLock(memory);
  if (!destination) {
    GlobalFree(memory);
    if (error) *error = windowsError(QStringLiteral("无法锁定剪贴板内存"));
    return false;
  }
  if (!bytes.isEmpty()) std::memcpy(destination, bytes.constData(), static_cast<size_t>(bytes.size()));
  GlobalUnlock(memory);
  if (!SetClipboardData(format, memory)) {
    GlobalFree(memory);
    if (error) *error = windowsError(QStringLiteral("无法写入系统剪贴板"));
    return false;
  }
  // Ownership transfers to Windows after SetClipboardData succeeds.
  return true;
}

QImage decodeSafePng(const QByteArray& bytes) {
  if (bytes.isEmpty() || bytes.size() > 16 * 1024 * 1024) return {};
  QBuffer buffer;
  buffer.setData(bytes);
  if (!buffer.open(QIODevice::ReadOnly)) return {};
  QImageReader reader(&buffer, "PNG");
  const QSize dimensions = reader.size();
  if (!dimensions.isValid() || dimensions.width() > 16384 || dimensions.height() > 16384 ||
      static_cast<qint64>(dimensions.width()) * dimensions.height() > 40'000'000) return {};
  return reader.read();
}

QByteArray dibToPng(const QByteArray& dib) {
  if (dib.size() < static_cast<qsizetype>(sizeof(BITMAPINFOHEADER))) return {};
  const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(dib.constData());
  if (header->biSize < sizeof(BITMAPINFOHEADER) || header->biSize > static_cast<DWORD>(dib.size())) return {};
  const qint64 width = std::abs(static_cast<qint64>(header->biWidth));
  const qint64 height = std::abs(static_cast<qint64>(header->biHeight));
  if (width < 1 || height < 1 || width > 16384 || height > 16384 ||
      width * height > 40'000'000) return {};
  qsizetype pixelOffset = static_cast<qsizetype>(header->biSize);
  if (header->biSize == sizeof(BITMAPINFOHEADER) &&
      (header->biCompression == BI_BITFIELDS || header->biCompression == kBiAlphaBitfields)) {
    pixelOffset += header->biCompression == kBiAlphaBitfields ? 16 : 12;
  }
  DWORD colors = header->biClrUsed;
  if (colors == 0 && header->biBitCount <= 8) colors = 1u << header->biBitCount;
  pixelOffset += static_cast<qsizetype>(colors) * 4;
  if (pixelOffset < 0 || pixelOffset >= dib.size()) return {};

  QByteArray bitmap(static_cast<qsizetype>(sizeof(BITMAPFILEHEADER)) + dib.size(), Qt::Uninitialized);
  auto* fileHeader = reinterpret_cast<BITMAPFILEHEADER*>(bitmap.data());
  std::memset(fileHeader, 0, sizeof(*fileHeader));
  fileHeader->bfType = 0x4d42;
  fileHeader->bfSize = static_cast<DWORD>(bitmap.size());
  fileHeader->bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + pixelOffset);
  std::memcpy(bitmap.data() + sizeof(BITMAPFILEHEADER), dib.constData(), static_cast<size_t>(dib.size()));
  const QImage image = QImage::fromData(bitmap, "BMP");
  if (image.isNull()) return {};
  QByteArray pngBytes;
  QBuffer buffer(&pngBytes);
  if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return {};
  return pngBytes;
}

QByteArray imageToDibV5(const QImage& source) {
  QImage image = source.convertToFormat(QImage::Format_ARGB32);
  if (image.isNull() || image.width() <= 0 || image.height() <= 0) return {};
  const qsizetype rowBytes = static_cast<qsizetype>(image.width()) * 4;
  const qsizetype pixelBytes = rowBytes * image.height();
  QByteArray result(static_cast<qsizetype>(sizeof(BITMAPV5HEADER)) + pixelBytes, Qt::Uninitialized);
  auto* header = reinterpret_cast<BITMAPV5HEADER*>(result.data());
  std::memset(header, 0, sizeof(*header));
  header->bV5Size = sizeof(BITMAPV5HEADER);
  header->bV5Width = image.width();
  header->bV5Height = -image.height();  // top-down DIB
  header->bV5Planes = 1;
  header->bV5BitCount = 32;
  header->bV5Compression = BI_BITFIELDS;
  header->bV5SizeImage = static_cast<DWORD>(pixelBytes);
  header->bV5RedMask = 0x00ff0000;
  header->bV5GreenMask = 0x0000ff00;
  header->bV5BlueMask = 0x000000ff;
  header->bV5AlphaMask = 0xff000000;
  header->bV5CSType = 0x73524742;  // LCS_sRGB without a compiler multi-char warning.
  header->bV5Intent = LCS_GM_IMAGES;
  char* pixels = result.data() + sizeof(BITMAPV5HEADER);
  for (int row = 0; row < image.height(); ++row) {
    std::memcpy(pixels + static_cast<qsizetype>(row) * rowBytes, image.constScanLine(row),
                static_cast<size_t>(rowBytes));
  }
  return result;
}

bool keySequenceToNative(const QKeySequence& sequence, UINT* modifiers, UINT* key) {
  if (sequence.isEmpty()) return false;
  const QKeyCombination combination = sequence[0];
  const auto qtModifiers = combination.keyboardModifiers();
  UINT nativeModifiers = MOD_NOREPEAT;
  if (qtModifiers.testFlag(Qt::ControlModifier)) nativeModifiers |= MOD_CONTROL;
  if (qtModifiers.testFlag(Qt::ShiftModifier)) nativeModifiers |= MOD_SHIFT;
  if (qtModifiers.testFlag(Qt::AltModifier)) nativeModifiers |= MOD_ALT;
  if (qtModifiers.testFlag(Qt::MetaModifier)) nativeModifiers |= MOD_WIN;
  int qtKey = combination.key();
  UINT virtualKey = 0;
  if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) virtualKey = static_cast<UINT>('A' + qtKey - Qt::Key_A);
  else if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9) virtualKey = static_cast<UINT>('0' + qtKey - Qt::Key_0);
  else if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F24) virtualKey = VK_F1 + qtKey - Qt::Key_F1;
  if (virtualKey == 0 || nativeModifiers == MOD_NOREPEAT) return false;
  *modifiers = nativeModifiers;
  *key = virtualKey;
  return true;
}

}  // namespace

WindowsClipboard::WindowsClipboard(QObject* parent) : QObject(parent) {}

WindowsClipboard::~WindowsClipboard() {
  if (window_) {
    if (monitoring_) RemoveClipboardFormatListener(window_);
    if (hotkeyRegistered_) UnregisterHotKey(window_, kHistoryHotkeyId);
    DestroyWindow(window_);
  }
  if (windowClass_) UnregisterClassW(kWindowClassName, GetModuleHandleW(nullptr));
}

bool WindowsClipboard::initialize(QString* error) {
  if (window_) return true;
  WNDCLASSEXW windowClass{};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc = &WindowsClipboard::windowProcedure;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = kWindowClassName;
  windowClass_ = RegisterClassExW(&windowClass);
  if (!windowClass_ && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    if (error) *error = windowsError(QStringLiteral("无法注册剪贴板消息窗口"));
    return false;
  }
  window_ = CreateWindowExW(0, kWindowClassName, L"DropSpace Clipboard", 0, 0, 0, 0, 0,
                            HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), this);
  if (!window_) {
    if (error) *error = windowsError(QStringLiteral("无法创建剪贴板消息窗口"));
    return false;
  }
  markerFormat_ = RegisterClipboardFormatW(kMarkerFormatName);
  pngFormat_ = RegisterClipboardFormatW(L"PNG");
  if (!markerFormat_ || !pngFormat_) {
    if (error) *error = windowsError(QStringLiteral("无法注册剪贴板格式"));
    return false;
  }
  return true;
}

bool WindowsClipboard::setMonitoringEnabled(bool enabled, QString* error) {
  if (!window_ && !initialize(error)) return false;
  if (enabled == monitoring_) return true;
  if (enabled) {
    if (!AddClipboardFormatListener(window_)) {
      if (error) *error = windowsError(QStringLiteral("无法启用剪贴板监听"));
      return false;
    }
    lastSequence_ = GetClipboardSequenceNumber();
    monitoring_ = true;
  } else {
    if (!RemoveClipboardFormatListener(window_)) {
      if (error) *error = windowsError(QStringLiteral("无法停用剪贴板监听"));
      return false;
    }
    monitoring_ = false;
  }
  return true;
}

bool WindowsClipboard::setHistoryHotkey(const QKeySequence& sequence, QString* error) {
  if (!window_ && !initialize(error)) return false;
  UINT modifiers = 0;
  UINT key = 0;
  if (!keySequenceToNative(sequence, &modifiers, &key)) {
    if (error) *error = QStringLiteral("快捷键必须包含 Ctrl、Shift、Alt 或 Win，并使用字母、数字或 F1-F24");
    return false;
  }
  const UINT previousModifiers = hotkeyModifiers_;
  const UINT previousKey = hotkeyKey_;
  if (hotkeyRegistered_) {
    UnregisterHotKey(window_, kHistoryHotkeyId);
    hotkeyRegistered_ = false;
  }
  if (!RegisterHotKey(window_, kHistoryHotkeyId, modifiers, key)) {
    if (previousKey != 0 && RegisterHotKey(window_, kHistoryHotkeyId, previousModifiers, previousKey)) {
      hotkeyRegistered_ = true;
    }
    if (error) *error = QStringLiteral("快捷键已被其他程序占用");
    return false;
  }
  hotkeyRegistered_ = true;
  hotkeyModifiers_ = modifiers;
  hotkeyKey_ = key;
  return true;
}

bool WindowsClipboard::writeRemote(const QString& clipboardId, const QString& contentHash,
                                   const QString& contentType, const QByteArray& content,
                                   QString* error) {
  const QByteArray marker = QByteArray("v1\n") + clipboardId.toUtf8() + '\n' + contentHash.toLatin1() + '\n';
  if (contentType == QStringLiteral("text")) return writeText(content, marker, error);
  if (contentType == QStringLiteral("image/png")) return writeImage(content, marker, error);
  if (error) *error = QStringLiteral("不支持的远端剪贴板类型");
  return false;
}

bool WindowsClipboard::writeText(const QByteArray& utf8, const QByteArray& marker, QString* error) {
  const QString text = QString::fromUtf8(utf8);
  QByteArray unicode((text.size() + 1) * static_cast<qsizetype>(sizeof(wchar_t)), Qt::Uninitialized);
  std::memcpy(unicode.data(), text.utf16(), static_cast<size_t>(text.size()) * sizeof(char16_t));
  std::memset(unicode.data() + text.size() * sizeof(wchar_t), 0, sizeof(wchar_t));
  if (!OpenClipboard(window_)) {
    if (error) *error = windowsError(QStringLiteral("系统剪贴板正被其他程序占用"));
    return false;
  }
  const bool emptied = EmptyClipboard();
  bool success = emptied && setGlobalClipboardData(markerFormat_, marker, error) &&
                 setGlobalClipboardData(CF_UNICODETEXT, unicode, error);
  CloseClipboard();
  if (!emptied && error) *error = windowsError(QStringLiteral("无法清空系统剪贴板"));
  return success;
}

bool WindowsClipboard::writeImage(const QByteArray& pngBytes, const QByteArray& marker, QString* error) {
  const QImage image = decodeSafePng(pngBytes);
  const QByteArray dib = imageToDibV5(image);
  if (image.isNull() || dib.isEmpty()) {
    if (error) *error = QStringLiteral("远端 PNG 图片无效");
    return false;
  }
  if (!OpenClipboard(window_)) {
    if (error) *error = windowsError(QStringLiteral("系统剪贴板正被其他程序占用"));
    return false;
  }
  const bool emptied = EmptyClipboard();
  bool success = emptied && setGlobalClipboardData(markerFormat_, marker, error) &&
                 setGlobalClipboardData(pngFormat_, pngBytes, error) &&
                 setGlobalClipboardData(CF_DIBV5, dib, error);
  CloseClipboard();
  if (!emptied && error) *error = windowsError(QStringLiteral("无法清空系统剪贴板"));
  return success;
}

LRESULT CALLBACK WindowsClipboard::windowProcedure(HWND window, UINT message, WPARAM wParam,
                                                    LPARAM lParam) {
  auto* self = reinterpret_cast<WindowsClipboard*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
    self = static_cast<WindowsClipboard*>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    if (self) self->window_ = window;
  }
  return self ? self->handleMessage(message, wParam, lParam)
              : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT WindowsClipboard::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_CLIPBOARDUPDATE && monitoring_) {
    const DWORD sequence = GetClipboardSequenceNumber();
    if (sequence != 0 && sequence != lastSequence_) {
      lastSequence_ = sequence;
      QTimer::singleShot(0, this, [this, sequence] { captureClipboard(sequence); });
    }
    return 0;
  }
  if (message == WM_HOTKEY && static_cast<int>(wParam) == kHistoryHotkeyId) {
    emit historyHotkeyActivated();
    return 0;
  }
  return DefWindowProcW(window_, message, wParam, lParam);
}

void WindowsClipboard::captureClipboard(DWORD expectedSequence, int attempt) {
  if (!monitoring_ || expectedSequence != lastSequence_) return;
  QString contentType;
  QByteArray content;
  bool isDropSpaceWrite = false;
  QString error;
  if (!readClipboard(&contentType, &content, &isDropSpaceWrite, &error)) {
    if (GetLastError() == ERROR_ACCESS_DENIED && attempt < 5) {
      QTimer::singleShot(20 * (attempt + 1), this,
                         [this, expectedSequence, attempt] { captureClipboard(expectedSequence, attempt + 1); });
      return;
    }
    if (!error.isEmpty()) emit clipboardError(error);
    return;
  }
  if (isDropSpaceWrite || content.isEmpty()) return;
  emit localContentCaptured(contentType, content, QDateTime::currentDateTimeUtc());
}

bool WindowsClipboard::readClipboard(QString* contentType, QByteArray* content,
                                     bool* isDropSpaceWrite, QString* error) const {
  if (!OpenClipboard(window_)) {
    if (error) *error = windowsError(QStringLiteral("读取剪贴板失败"));
    return false;
  }
  *isDropSpaceWrite = false;
  if (IsClipboardFormatAvailable(markerFormat_)) {
    const QByteArray marker = globalBytes(GetClipboardData(markerFormat_));
    *isDropSpaceWrite = marker.startsWith("v1\n") && marker.count('\n') >= 3;
  }
  if (*isDropSpaceWrite) {
    CloseClipboard();
    return true;
  }

  if (IsClipboardFormatAvailable(pngFormat_)) {
    const QByteArray pngBytes = globalBytes(GetClipboardData(pngFormat_));
    if (!decodeSafePng(pngBytes).isNull()) {
      *contentType = QStringLiteral("image/png");
      *content = pngBytes;
      CloseClipboard();
      return true;
    }
  }
  for (UINT format : {static_cast<UINT>(CF_DIBV5), static_cast<UINT>(CF_DIB)}) {
    if (!IsClipboardFormatAvailable(format)) continue;
    const QByteArray pngBytes = dibToPng(globalBytes(GetClipboardData(format)));
    if (!pngBytes.isEmpty()) {
      *contentType = QStringLiteral("image/png");
      *content = pngBytes;
      CloseClipboard();
      return true;
    }
  }
  if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
    HGLOBAL handle = GetClipboardData(CF_UNICODETEXT);
    const wchar_t* value = handle ? static_cast<const wchar_t*>(GlobalLock(handle)) : nullptr;
    if (value) {
      const SIZE_T bytes = GlobalSize(handle);
      const size_t maximum = bytes / sizeof(wchar_t);
      size_t length = 0;
      while (length < maximum && value[length] != L'\0') ++length;
      *contentType = QStringLiteral("text");
      *content = QString::fromWCharArray(value, static_cast<qsizetype>(length)).toUtf8();
      GlobalUnlock(handle);
      CloseClipboard();
      return true;
    }
  }
  CloseClipboard();
  return true;  // Unsupported clipboard formats are intentionally ignored.
}

}  // namespace dropspace::windows

