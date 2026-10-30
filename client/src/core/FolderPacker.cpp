#include "core/FolderPacker.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStringList>

#include <QtCore/private/qzipwriter_p.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <limits>
#include <vector>

namespace dropspace {
namespace {

struct FrozenEntry {
  QString absolutePath;
  QString relativePath;
  bool directory = false;
  qint64 size = 0;
  qint64 modifiedMs = 0;
};

struct TreeRecord {
  QByteArray path;
  char kind = 'D';
  qint64 size = 0;
  QByteArray hash;
};

class HashingFile final : public QIODevice {
 public:
  explicit HashingFile(QString filename) : file_(std::move(filename)) {}

  bool open() {
    if (!file_.open(QIODevice::ReadOnly)) return false;
    return QIODevice::open(QIODevice::ReadOnly);
  }
  [[nodiscard]] QByteArray digest() const { return hash_.result().toHex(); }
  [[nodiscard]] qint64 bytesRead() const { return bytesRead_; }
  [[nodiscard]] QString errorString() const { return file_.errorString(); }
  [[nodiscard]] bool isSequential() const override { return false; }
  [[nodiscard]] qint64 size() const override { return file_.size(); }
  [[nodiscard]] qint64 pos() const override { return file_.pos(); }
  bool seek(qint64 position) override {
    if (position != file_.pos()) return false;  // ZIP writer must consume exactly once.
    return true;
  }

 protected:
  qint64 readData(char* data, qint64 maximum) override {
    const qint64 count = file_.read(data, maximum);
    if (count > 0) {
      hash_.addData(QByteArrayView(data, count));
      bytesRead_ += count;
    }
    return count;
  }
  qint64 writeData(const char*, qint64) override { return -1; }

 private:
  QFile file_;
  QCryptographicHash hash_{QCryptographicHash::Sha256};
  qint64 bytesRead_ = 0;
};

bool isReparsePoint(const QString& path) {
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  Q_UNUSED(path);
  return false;
#endif
}

bool safeComponent(const QString& component, QString* reason) {
  if (component.isEmpty() || component == QStringLiteral(".") ||
      component == QStringLiteral("..")) {
    *reason = QStringLiteral("empty or dot path component");
    return false;
  }
  if (component.endsWith(u'.') || component.endsWith(u' ')) {
    *reason = QStringLiteral("trailing dot or space");
    return false;
  }
  static const QString invalid = QStringLiteral("<>:\"/\\|?*");
  for (const QChar character : component) {
    if (character.unicode() < 32 || invalid.contains(character)) {
      *reason = QStringLiteral("Windows-invalid character");
      return false;
    }
  }
  QString base = component.section(u'.', 0, 0).toUpper();
  static const QSet<QString> reserved = {
      QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
      QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
      QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
      QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
      QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
      QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
      QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
      QStringLiteral("LPT9")};
  if (reserved.contains(base)) {
    *reason = QStringLiteral("Windows reserved name");
    return false;
  }
  return true;
}

bool scan(const QString& root, const FolderLimits& limits,
          std::vector<FrozenEntry>* entries, QString* error) {
  QHash<QString, QString> collisionKeys;
  QStringList pending{QString()};
  qint64 totalBytes = 0;
  while (!pending.empty()) {
    const QString parentRelative = pending.takeLast();
    const QString parentAbsolute = parentRelative.isEmpty()
                                       ? root
                                       : QDir(root).filePath(parentRelative);
    const auto children = QDir(parentAbsolute).entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDir::Name | QDir::DirsFirst);
    for (const auto& info : children) {
      const QString relative = parentRelative.isEmpty()
                                   ? info.fileName()
                                   : parentRelative + u'/' + info.fileName();
      QString reason;
      if (!safeComponent(info.fileName(), &reason)) {
        *error = QStringLiteral("Unsafe name %1: %2").arg(relative, reason);
        return false;
      }
      if (info.isSymLink() || isReparsePoint(info.absoluteFilePath())) {
        *error = QStringLiteral("Link or reparse point rejected: %1").arg(relative);
        return false;
      }
      if (!info.isDir() && !info.isFile()) {
        *error = QStringLiteral("Special filesystem object rejected: %1").arg(relative);
        return false;
      }
      const int depth = relative.count(u'/') + 1;
      if (depth > limits.maxDepth) {
        *error = QStringLiteral("Folder depth limit exceeded: %1").arg(relative);
        return false;
      }
      const QString canonical = relative.normalized(QString::NormalizationForm_C).toCaseFolded();
      if (const auto found = collisionKeys.constFind(canonical); found != collisionKeys.cend()) {
        *error = QStringLiteral("Case or Unicode-normalization conflict: %1 and %2")
                     .arg(*found, relative);
        return false;
      }
      collisionKeys.insert(canonical, relative);
      FrozenEntry entry{info.absoluteFilePath(),
                        relative.normalized(QString::NormalizationForm_C),
                        info.isDir(), info.isFile() ? info.size() : 0,
                        info.lastModified().toMSecsSinceEpoch()};
      if (!entry.directory) {
        if (entry.size < 0 || entry.size > limits.maxExpandedBytes - totalBytes) {
          *error = QStringLiteral("Expanded folder byte limit exceeded at %1").arg(relative);
          return false;
        }
        totalBytes += entry.size;
      }
      entries->push_back(std::move(entry));
      if (entries->size() > static_cast<std::size_t>(limits.maxEntries)) {
        *error = QStringLiteral("Folder entry limit exceeded");
        return false;
      }
      if (info.isDir()) pending.push_back(relative);
    }
  }
  std::sort(entries->begin(), entries->end(), [](const auto& left, const auto& right) {
    return left.relativePath.toUtf8() < right.relativePath.toUtf8();
  });
  return true;
}

QByteArray treeDigest(std::vector<TreeRecord> records) {
  std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
    return left.path < right.path;
  });
  QCryptographicHash digest(QCryptographicHash::Sha256);
  for (const auto& record : records) {
    digest.addData(QByteArrayView(&record.kind, 1));
    digest.addData(QByteArrayView("\0", 1));
    digest.addData(record.path);
    digest.addData(QByteArrayView("\0", 1));
    if (record.kind == 'F') {
      digest.addData(QByteArray::number(record.size));
      digest.addData(QByteArrayView("\0", 1));
      digest.addData(record.hash);
      digest.addData(QByteArrayView("\0", 1));
    }
  }
  return digest.result().toHex();
}

QByteArray fileDigest(const QString& filename, QString* error) {
  QFile input(filename);
  if (!input.open(QIODevice::ReadOnly)) {
    *error = input.errorString();
    return {};
  }
  QCryptographicHash digest(QCryptographicHash::Sha256);
  if (!digest.addData(&input)) {
    *error = input.errorString();
    return {};
  }
  return digest.result().toHex();
}

}  // namespace

bool packFolder(const QString& sourceRoot, const QString& outputZipPart,
                PackedFolder* result, QString* error, FolderLimits limits) {
  if (!result || !error) return false;
  *result = {};
  error->clear();
  const QFileInfo rootInfo(sourceRoot);
  if (!rootInfo.isDir() || rootInfo.isSymLink() || isReparsePoint(rootInfo.absoluteFilePath())) {
    *error = QStringLiteral("Source root is not a safe local directory");
    return false;
  }
  std::vector<FrozenEntry> entries;
  if (!scan(rootInfo.absoluteFilePath(), limits, &entries, error)) return false;

  QFile::remove(outputZipPart);
  QZipWriter writer(outputZipPart, QIODevice::WriteOnly | QIODevice::NewOnly);
  writer.setCompressionPolicy(QZipWriter::AutoCompress);
  if (!writer.isWritable()) {
    *error = QStringLiteral("Cannot create ZIP staging object");
    return false;
  }
  std::vector<TreeRecord> tree;
  qint64 expanded = 0;
  for (const auto& entry : entries) {
    const QFileInfo before(entry.absolutePath);
    if (before.isSymLink() || isReparsePoint(entry.absolutePath) ||
        before.isDir() != entry.directory ||
        (!entry.directory && before.size() != entry.size) ||
        before.lastModified().toMSecsSinceEpoch() != entry.modifiedMs) {
      *error = QStringLiteral("Folder changed while packing: %1").arg(entry.relativePath);
      writer.close();
      QFile::remove(outputZipPart);
      return false;
    }
    if (entry.directory) {
      writer.addDirectory(entry.relativePath);
      tree.push_back({entry.relativePath.toUtf8(), 'D', 0, {}});
    } else {
      HashingFile input(entry.absolutePath);
      if (!input.open()) {
        *error = QStringLiteral("Cannot read %1: %2").arg(entry.relativePath, input.errorString());
        writer.close();
        QFile::remove(outputZipPart);
        return false;
      }
      writer.addFile(entry.relativePath, &input);
      const QFileInfo after(entry.absolutePath);
      if (input.bytesRead() != entry.size || after.size() != entry.size ||
          after.lastModified().toMSecsSinceEpoch() != entry.modifiedMs) {
        *error = QStringLiteral("File changed while packing: %1").arg(entry.relativePath);
        writer.close();
        QFile::remove(outputZipPart);
        return false;
      }
      tree.push_back({entry.relativePath.toUtf8(), 'F', entry.size, input.digest()});
      expanded += entry.size;
    }
    if (writer.status() != QZipWriter::NoError) {
      *error = QStringLiteral("ZIP writer failed at %1").arg(entry.relativePath);
      writer.close();
      QFile::remove(outputZipPart);
      return false;
    }
  }
  writer.close();
  if (writer.status() != QZipWriter::NoError) {
    *error = QStringLiteral("ZIP finalization failed");
    QFile::remove(outputZipPart);
    return false;
  }

  // Detect additions/removals after the frozen scan, not merely mutations of
  // files that were already known.
  std::vector<FrozenEntry> afterEntries;
  if (!scan(rootInfo.absoluteFilePath(), limits, &afterEntries, error) ||
      afterEntries.size() != entries.size()) {
    if (error->isEmpty()) *error = QStringLiteral("Folder membership changed while packing");
    QFile::remove(outputZipPart);
    return false;
  }
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (entries[index].relativePath != afterEntries[index].relativePath ||
        entries[index].directory != afterEntries[index].directory ||
        entries[index].size != afterEntries[index].size ||
        entries[index].modifiedMs != afterEntries[index].modifiedMs) {
      *error = QStringLiteral("Folder changed while packing: %1").arg(entries[index].relativePath);
      QFile::remove(outputZipPart);
      return false;
    }
  }

  const QByteArray objectHash = fileDigest(outputZipPart, error);
  if (objectHash.isEmpty() && QFileInfo(outputZipPart).size() != 0) {
    QFile::remove(outputZipPart);
    return false;
  }
  *result = {outputZipPart, QString::fromLatin1(objectHash),
             QString::fromLatin1(treeDigest(std::move(tree))),
             QFileInfo(outputZipPart).size(), expanded,
             static_cast<std::int64_t>(entries.size())};
  return true;
}

}  // namespace dropspace

