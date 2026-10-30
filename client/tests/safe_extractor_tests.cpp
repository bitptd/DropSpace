#include "core/FolderPacker.h"
#include "core/SafeExtractor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtCore/private/qzipwriter_p.h>
#include <iostream>

using namespace dropspace;

int fail(const QString& message) { std::cerr << message.toStdString() << '\n'; return 1; }
bool writeFile(const QString& path, const QByteArray& bytes) {
  QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv); QTemporaryDir temp;
  if (!temp.isValid()) return fail("temp failed");
  const QString source = QDir(temp.path()).filePath(QStringLiteral("源"));
  QDir().mkpath(QDir(source).filePath(QStringLiteral("嵌套/空")));
  if (!writeFile(QDir(source).filePath(QStringLiteral("嵌套/内容.txt")), "payload")) return fail("write failed");
  PackedFolder packed; QString error; const QString zip = QDir(temp.path()).filePath("object.zip");
  if (!packFolder(source, zip, &packed, &error)) return fail(error);
  std::vector<ExtractedEntry> manifest; const QString output = QDir(temp.path()).filePath(QStringLiteral("交付"));
  if (!safeExtractZip(zip, output, &manifest, &error)) return fail(error);
  QFile restored(QDir(output).filePath(QStringLiteral("嵌套/内容.txt")));
  if (!restored.open(QIODevice::ReadOnly) || restored.readAll() != "payload") return fail("roundtrip mismatch");
  if (!QFileInfo(QDir(output).filePath(QStringLiteral("嵌套/空"))).isDir()) return fail("empty directory lost");

  const QString malicious = QDir(temp.path()).filePath("bad.zip");
  QZipWriter bad(malicious); bad.addFile(QStringLiteral("../escape.txt"), QByteArray("bad")); bad.close();
  const QString rejected = QDir(temp.path()).filePath("rejected");
  if (safeExtractZip(malicious, rejected, &manifest, &error)) return fail("traversal accepted");
  if (QFileInfo::exists(rejected) || QFileInfo::exists(QDir(temp.path()).filePath("escape.txt"))) return fail("partial output leaked");

  const QString conflict = QDir(temp.path()).filePath("conflict.zip");
  QZipWriter collision(conflict); collision.addFile(QStringLiteral("A.txt"), QByteArray("a"));
  collision.addFile(QStringLiteral("a.txt"), QByteArray("b")); collision.close();
  if (safeExtractZip(conflict, rejected, &manifest, &error)) return fail("case collision accepted");
  std::cout << "safe extractor tests passed\n"; return 0;
}
