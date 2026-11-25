/** @fileoverview Registers saved screenshots in the desktop's recent files. */
#include "desktop-recent.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QXmlStreamReader>

namespace {
// GLib's GBookmarkFile writes this header and these namespace prefixes; the
// entries below use the same prefixes, so a list GLib wrote stays readable by
// GLib after the splice.
constexpr char kXbelHeader[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<xbel version=\"1.0\"\n"
    "      xmlns:bookmark=\"http://www.freedesktop.org/standards/desktop-bookmarks\"\n"
    "      xmlns:mime=\"http://www.freedesktop.org/standards/shared-mime-info\"\n"
    ">\n";
constexpr char kXbelFooter[] = "</xbel>";

bool isXbelDocument(const QByteArray &contents) {
  QXmlStreamReader reader(contents);
  bool sawRoot = false;
  while (!reader.atEnd()) {
    reader.readNext();
    if (!sawRoot && reader.isStartElement()) {
      if (reader.name() != QLatin1String("xbel"))
        return false;
      sawRoot = true;
    }
  }
  return sawRoot && !reader.hasError();
}

// Drops an earlier entry for the same file so the list keeps one bookmark per
// href, as GLib requires.
void removeBookmark(QByteArray &contents, const QByteArray &escapedHref) {
  const QByteArray start = "<bookmark href=\"" + escapedHref + '"';
  const qsizetype begin = contents.indexOf(start);
  if (begin < 0)
    return;
  const qsizetype tagEnd = contents.indexOf('>', begin);
  if (tagEnd < 0)
    return;
  qsizetype end = tagEnd + 1;
  if (contents.at(tagEnd - 1) != '/') {
    const QByteArray close = "</bookmark>";
    const qsizetype closeAt = contents.indexOf(close, tagEnd);
    if (closeAt < 0)
      return;
    end = closeAt + close.size();
  }
  qsizetype lineStart = begin;
  while (lineStart > 0 && contents.at(lineStart - 1) == ' ')
    --lineStart;
  if (end < contents.size() && contents.at(end) == '\n')
    ++end;
  contents.remove(lineStart, end - lineStart);
}

QByteArray bookmarkEntry(const QByteArray &escapedHref,
                         const QByteArray &mimeType) {
  const QByteArray now =
      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toUtf8();
  return "  <bookmark href=\"" + escapedHref + "\" added=\"" + now +
         "\" modified=\"" + now + "\" visited=\"" + now +
         "\">\n"
         "    <info>\n"
         "      <metadata owner=\"http://freedesktop.org\">\n"
         "        <mime:mime-type type=\"" + mimeType + "\"/>\n"
         "        <bookmark:applications>\n"
         "          <bookmark:application name=\"Omasnap\" "
         "exec=\"&apos;omasnap %u&apos;\" modified=\"" + now +
         "\" count=\"1\"/>\n"
         "        </bookmark:applications>\n"
         "      </metadata>\n"
         "    </info>\n"
         "  </bookmark>\n";
}
} // namespace

QString desktopRecentFilesPath() {
  return QDir(QStandardPaths::writableLocation(
                  QStandardPaths::GenericDataLocation))
      .filePath(QStringLiteral("recently-used.xbel"));
}

bool addToDesktopRecentFiles(const QString &path, QString &error) {
  const QFileInfo file(path);
  if (!file.isFile()) {
    error = QStringLiteral("Recent file does not exist: %1").arg(path);
    return false;
  }
  const QByteArray href =
      QUrl::fromLocalFile(file.absoluteFilePath()).toEncoded();
  const QByteArray escapedHref = QString::fromUtf8(href).toHtmlEscaped().toUtf8();
  const QByteArray mimeType =
      QMimeDatabase().mimeTypeForFile(file).name().toHtmlEscaped().toUtf8();

  // Saves run on pool threads; serialize this process's read-modify-write.
  static QMutex mutex;
  const QMutexLocker locker(&mutex);

  const QString listPath = desktopRecentFilesPath();
  QByteArray contents;
  if (QFile existing(listPath); existing.exists()) {
    if (!existing.open(QIODevice::ReadOnly)) {
      error = QStringLiteral("Could not read %1: %2")
                  .arg(listPath, existing.errorString());
      return false;
    }
    contents = existing.readAll();
  }
  if (contents.trimmed().isEmpty()) {
    contents = QByteArray(kXbelHeader) + kXbelFooter + '\n';
  } else if (!isXbelDocument(contents)) {
    error = QStringLiteral("Not an XBEL recent files list: %1").arg(listPath);
    return false;
  }

  removeBookmark(contents, escapedHref);
  const qsizetype footer = contents.lastIndexOf(kXbelFooter);
  contents.insert(footer, bookmarkEntry(escapedHref, mimeType));
  // A list with other namespace prefixes would no longer parse; keep it.
  if (!isXbelDocument(contents)) {
    error = QStringLiteral("Could not add an entry to %1").arg(listPath);
    return false;
  }

  if (!QDir().mkpath(QFileInfo(listPath).absolutePath())) {
    error = QStringLiteral("Could not create %1").arg(listPath);
    return false;
  }
  QSaveFile out(listPath);
  if (!out.open(QIODevice::WriteOnly) ||
      !out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
      out.write(contents) != contents.size() || !out.commit()) {
    error = QStringLiteral("Could not write %1: %2")
                .arg(listPath, out.errorString());
    return false;
  }
  return true;
}
