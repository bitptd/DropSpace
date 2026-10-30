#include "core/FolderPacker.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <iostream>

namespace {

bool write(const QString& filename, const QByteArray& bytes) {
  QFile file(filename);
  return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size();
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir temporary;
  if (!temporary.isValid()) return 1;
  const QString root = temporary.filePath(QStringLiteral("源文件夹"));
  if (!QDir().mkpath(root + QStringLiteral("/nested/空目录")) ||
      !write(root + QStringLiteral("/nested/内容.txt"), QByteArray("payload"))) return 2;

  dropspace::PackedFolder first;
  QString error;
  const QString firstZip = temporary.filePath(QStringLiteral("first.zip.part"));
  if (!dropspace::packFolder(root, firstZip, &first, &error)) {
    std::cerr << error.toStdString() << '\n';
    return 3;
  }
  if (first.expandedSizeBytes != 7 || first.entryCount != 3 ||
      first.objectSha256.size() != 64 || first.treeSha256.size() != 64) return 4;

  dropspace::PackedFolder second;
  const QString secondZip = temporary.filePath(QStringLiteral("second.zip.part"));
  if (!dropspace::packFolder(root, secondZip, &second, &error)) return 5;
  // Container bytes may vary; the logical tree identity must not.
  if (first.treeSha256 != second.treeSha256) return 6;

  const QString fixtureOutput = qEnvironmentVariable("DROPSPACE_FIXTURE_OUTPUT");
  if (!fixtureOutput.isEmpty()) {
    QFile::remove(fixtureOutput);
    if (!QFile::copy(firstZip, fixtureOutput)) return 9;
    const QJsonObject metadata{
        {QStringLiteral("object_size_bytes"), first.objectSizeBytes},
        {QStringLiteral("object_sha256"), first.objectSha256},
        {QStringLiteral("expanded_size_bytes"), first.expandedSizeBytes},
        {QStringLiteral("entry_count"), first.entryCount},
        {QStringLiteral("tree_sha256"), first.treeSha256}};
    std::cout << QJsonDocument(metadata).toJson(QJsonDocument::Compact).constData() << '\n';
  }

  const QString unsafe = temporary.filePath(QStringLiteral("unsafe"));
  QDir().mkpath(unsafe);
  if (!write(unsafe + QStringLiteral("/CON.txt"), QByteArray("x"))) return 7;
  dropspace::PackedFolder rejected;
  if (dropspace::packFolder(unsafe, temporary.filePath(QStringLiteral("unsafe.zip")),
                            &rejected, &error)) return 8;
  std::cout << "folder_packer_tests: passed\n";
  return 0;
}

