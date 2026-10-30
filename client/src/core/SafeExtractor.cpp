#include "core/SafeExtractor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QUuid>
#include <zlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <array>

namespace dropspace {
namespace {

quint16 u16(const char* p) { return quint16(quint8(p[0])) | (quint16(quint8(p[1])) << 8); }
quint32 u32(const char* p) { return quint32(u16(p)) | (quint32(u16(p + 2)) << 16); }
quint64 u64(const char* p) { return quint64(u32(p)) | (quint64(u32(p + 4)) << 32); }

struct Entry {
  QString path;
  QByteArray rawName;
  quint16 flags = 0, method = 0;
  quint32 crc = 0;
  quint64 compressed = 0, expanded = 0, localOffset = 0;
  bool directory = false;
};

bool isReparse(const QString& path) {
#ifdef _WIN32
  const DWORD a = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
  Q_UNUSED(path); return false;
#endif
}

bool safeComponent(const QString& c, QString* error) {
  if (c.isEmpty() || c == QStringLiteral(".") || c == QStringLiteral("..") ||
      c.endsWith(u'.') || c.endsWith(u' ')) {
    *error = QStringLiteral("unsafe empty, dot, or trailing path component"); return false;
  }
  static const QString invalid = QStringLiteral("<>:\"/\\|?*");
  for (const QChar ch : c) if (ch.unicode() < 32 || invalid.contains(ch)) {
    *error = QStringLiteral("Windows-invalid path component"); return false;
  }
  const QString base = c.section(u'.', 0, 0).toUpper();
  static const QSet<QString> reserved = {
      "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
      "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
      "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
  if (reserved.contains(base)) { *error = QStringLiteral("Windows reserved name"); return false; }
  return true;
}

bool zip64(const QByteArray& extra, quint32 c32, quint32 e32, quint32 o32,
           quint64* compressed, quint64* expanded, quint64* offset) {
  qsizetype pos = 0;
  while (pos + 4 <= extra.size()) {
    const quint16 id = u16(extra.constData() + pos), n = u16(extra.constData() + pos + 2);
    pos += 4;
    if (pos + n > extra.size()) return false;
    if (id == 1) {
      const char* p = extra.constData() + pos; qsizetype left = n;
      auto take = [&](quint64* v) { if (left < 8) return false; *v = u64(p); p += 8; left -= 8; return true; };
      if (e32 == 0xffffffffU && !take(expanded)) return false;
      if (c32 == 0xffffffffU && !take(compressed)) return false;
      if (o32 == 0xffffffffU && !take(offset)) return false;
      return true;
    }
    pos += n;
  }
  return c32 != 0xffffffffU && e32 != 0xffffffffU && o32 != 0xffffffffU;
}

bool parse(QFile* file, const ExtractLimits& limits, std::vector<Entry>* entries, QString* error) {
  const qint64 size = file->size(), tailSize = std::min<qint64>(size, 65557);
  if (size < 22 || !file->seek(size - tailSize)) { *error = QStringLiteral("invalid ZIP size"); return false; }
  const QByteArray tail = file->read(tailSize);
  qint64 ep = -1;
  for (qint64 i = tail.size() - 22; i >= 0; --i) if (u32(tail.constData() + i) == 0x06054b50U) { ep = i; break; }
  if (ep < 0) { *error = QStringLiteral("ZIP end record missing"); return false; }
  quint64 count = u16(tail.constData() + ep + 10), centralSize = u32(tail.constData() + ep + 12),
          centralOffset = u32(tail.constData() + ep + 16);
  if (count == 0xffff || centralSize == 0xffffffffU || centralOffset == 0xffffffffU) {
    const qint64 absolute = size - tailSize + ep;
    if (absolute < 20 || !file->seek(absolute - 20)) { *error = QStringLiteral("ZIP64 locator missing"); return false; }
    const QByteArray locator = file->read(20);
    if (locator.size() != 20 || u32(locator.constData()) != 0x07064b50U ||
        !file->seek(qint64(u64(locator.constData() + 8)))) { *error = QStringLiteral("invalid ZIP64 locator"); return false; }
    const QByteArray record = file->read(56);
    if (record.size() < 56 || u32(record.constData()) != 0x06064b50U) { *error = QStringLiteral("invalid ZIP64 record"); return false; }
    count = u64(record.constData() + 32); centralSize = u64(record.constData() + 40); centralOffset = u64(record.constData() + 48);
  }
  if (count > quint64(limits.maxEntries) || centralOffset > quint64(size) ||
      centralSize > quint64(size) - centralOffset || !file->seek(qint64(centralOffset))) {
    *error = QStringLiteral("central directory exceeds limits"); return false;
  }
  QHash<QString, QString> collision; quint64 total = 0;
  for (quint64 i = 0; i < count; ++i) {
    const QByteArray h = file->read(46);
    if (h.size() != 46 || u32(h.constData()) != 0x02014b50U) { *error = QStringLiteral("invalid central entry"); return false; }
    Entry e; e.flags = u16(h.constData() + 8); e.method = u16(h.constData() + 10); e.crc = u32(h.constData() + 16);
    const quint32 c32 = u32(h.constData() + 20), e32 = u32(h.constData() + 24), o32 = u32(h.constData() + 42);
    e.compressed = c32; e.expanded = e32; e.localOffset = o32;
    const quint16 nl = u16(h.constData() + 28), xl = u16(h.constData() + 30), cl = u16(h.constData() + 32);
    const quint32 external = u32(h.constData() + 38);
    e.rawName = file->read(nl); const QByteArray extra = file->read(xl);
    if (e.rawName.size() != nl || extra.size() != xl || !file->seek(file->pos() + cl) ||
        !zip64(extra, c32, e32, o32, &e.compressed, &e.expanded, &e.localOffset)) {
      *error = QStringLiteral("truncated ZIP entry"); return false;
    }
    if ((e.flags & 1) || (e.flags & 0x40) || (e.method != 0 && e.method != 8)) {
      *error = QStringLiteral("encrypted or unsupported ZIP member"); return false;
    }
    e.path = QString::fromUtf8(e.rawName);
    if (e.path.toUtf8() != e.rawName || e.path.startsWith(u'/') || e.path.contains(u'\\') || e.path.contains(u':')) {
      *error = QStringLiteral("unsafe or non-UTF-8 member name"); return false;
    }
    e.directory = e.path.endsWith(u'/'); while (e.path.endsWith(u'/')) e.path.chop(1);
    if (e.path.isEmpty()) { *error = QStringLiteral("empty member name"); return false; }
    e.path = e.path.normalized(QString::NormalizationForm_C);
    const QStringList parts = e.path.split(u'/', Qt::KeepEmptyParts);
    if (parts.size() > limits.maxDepth) { *error = QStringLiteral("depth limit exceeded"); return false; }
    for (const auto& part : parts) if (!safeComponent(part, error)) return false;
    const QString key = e.path.toCaseFolded();
    if (collision.contains(key)) { *error = QStringLiteral("case or Unicode collision"); return false; }
    collision.insert(key, e.path);
    const quint32 type = (external >> 16) & 0170000U;
    if (type != 0 && type != 0100000U && type != 0040000U) { *error = QStringLiteral("link or special member rejected"); return false; }
    if (e.directory && e.expanded) { *error = QStringLiteral("directory has content"); return false; }
    if (!e.directory) {
      if (e.expanded > quint64(limits.maxExpandedBytes) - total) { *error = QStringLiteral("expanded byte limit exceeded"); return false; }
      total += e.expanded;
      if (e.expanded && (e.compressed == 0 || double(e.expanded) / double(e.compressed) > limits.maxCompressionRatio)) {
        *error = QStringLiteral("compression ratio limit exceeded"); return false;
      }
    }
    entries->push_back(std::move(e));
  }
  return true;
}

bool createParents(const QString& root, const QStringList& parts, QString* error) {
  QString current = root;
  for (int i = 0; i + 1 < parts.size(); ++i) {
    current = QDir(current).filePath(parts[i]); QFileInfo info(current);
    if (info.exists()) {
      if (!info.isDir() || info.isSymLink() || isReparse(current)) { *error = QStringLiteral("unsafe extraction parent"); return false; }
    } else if (!QDir().mkdir(current)) { *error = QStringLiteral("cannot create extraction parent"); return false; }
  }
  return true;
}

bool extractOne(QFile* zip, const Entry& e, const QString& path, qint64* cumulative,
                const ExtractLimits& limits, const std::function<bool()>& cancelled, QString* error) {
  if (!zip->seek(qint64(e.localOffset))) { *error = QStringLiteral("bad local offset"); return false; }
  const QByteArray h = zip->read(30);
  if (h.size() != 30 || u32(h.constData()) != 0x04034b50U) { *error = QStringLiteral("bad local header"); return false; }
  const quint16 nl = u16(h.constData() + 26), xl = u16(h.constData() + 28);
  if (zip->read(nl) != e.rawName || !zip->seek(zip->pos() + xl)) { *error = QStringLiteral("local member mismatch"); return false; }
  QFile out(path);
  if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { *error = out.errorString(); return false; }
  std::array<unsigned char, 65536> input{}, output{}; quint64 left = e.compressed, produced = 0;
  uLong crc = crc32(0, Z_NULL, 0); z_stream stream{}; int zresult = Z_OK;
  const bool initialized = e.method == 8 && inflateInit2(&stream, -MAX_WBITS) == Z_OK;
  if (e.method == 8 && !initialized) { *error = QStringLiteral("deflate initialization failed"); return false; }
  bool ok = true;
  while (left && ok) {
    if (cancelled && cancelled()) { *error = QStringLiteral("extraction cancelled"); ok = false; break; }
    const qint64 ask = qint64(std::min<quint64>(left, input.size())), got = zip->read(reinterpret_cast<char*>(input.data()), ask);
    if (got != ask) { *error = QStringLiteral("truncated member data"); ok = false; break; }
    left -= quint64(got);
    if (e.method == 0) {
      ok = out.write(reinterpret_cast<char*>(input.data()), got) == got;
      crc = crc32(crc, input.data(), uInt(got)); produced += quint64(got); *cumulative += got;
    } else {
      stream.next_in = input.data(); stream.avail_in = uInt(got);
      do {
        stream.next_out = output.data(); stream.avail_out = uInt(output.size()); zresult = inflate(&stream, Z_NO_FLUSH);
        if (zresult != Z_OK && zresult != Z_STREAM_END) { ok = false; break; }
        const auto n = output.size() - stream.avail_out;
        if (n) { ok = out.write(reinterpret_cast<char*>(output.data()), qint64(n)) == qint64(n); crc = crc32(crc, output.data(), uInt(n)); produced += n; *cumulative += qint64(n); }
        if (produced > e.expanded || *cumulative > limits.maxExpandedBytes) ok = false;
      } while (ok && stream.avail_in && zresult != Z_STREAM_END);
    }
  }
  if (initialized) inflateEnd(&stream); out.close();
  if (!ok || produced != e.expanded || quint32(crc) != e.crc || (e.method == 8 && zresult != Z_STREAM_END)) {
    if (error->isEmpty()) *error = QStringLiteral("member length, CRC, or stream validation failed"); return false;
  }
  return true;
}

}  // namespace

bool safeExtractZip(const QString& zipPath, const QString& destination,
                    std::vector<ExtractedEntry>* manifest, QString* error,
                    ExtractLimits limits, const std::function<bool()>& cancelled) {
  if (!manifest || !error) return false; manifest->clear(); error->clear();
  if (QFileInfo::exists(destination)) { *error = QStringLiteral("destination already exists"); return false; }
  QFile zip(zipPath); if (!zip.open(QIODevice::ReadOnly)) { *error = zip.errorString(); return false; }
  std::vector<Entry> entries; if (!parse(&zip, limits, &entries, error)) return false;
  const QFileInfo dest(destination); const QString parent = dest.absolutePath();
  if (!QDir().mkpath(parent)) { *error = QStringLiteral("cannot create destination parent"); return false; }
  const QString tempName = QStringLiteral(".%1.dropspace-%2.part").arg(dest.fileName(), QUuid::createUuid().toString(QUuid::WithoutBraces));
  const QString temp = QDir(parent).filePath(tempName);
  if (!QDir().mkdir(temp)) { *error = QStringLiteral("cannot create private extraction root"); return false; }
  qint64 cumulative = 0;
  for (const auto& e : entries) {
    if (cancelled && cancelled()) { *error = QStringLiteral("extraction cancelled"); break; }
    const QStringList parts = e.path.split(u'/'); if (!createParents(temp, parts, error)) break;
    const QString output = QDir(temp).filePath(e.path);
    if (e.directory) { if (!QDir().mkdir(output)) { *error = QStringLiteral("duplicate or invalid directory"); break; } }
    else if (!extractOne(&zip, e, output, &cumulative, limits, cancelled, error)) break;
    manifest->push_back({e.path, e.directory, qint64(e.expanded)});
  }
  bool success = false;
  if (manifest->size() == entries.size()) {
    success = QDir(parent).rename(tempName, dest.fileName());
    if (!success) *error = QStringLiteral("cannot atomically publish extracted directory");
  }
  if (!success) { QDir(temp).removeRecursively(); manifest->clear(); }
  return success;
}

}  // namespace dropspace
