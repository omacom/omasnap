/** @fileoverview Registers saved screenshots in the desktop's recent files. */
#include "desktop-recent.hpp"

#include "capture.hpp"

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

#include <sys/file.h>

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
constexpr int kWriteAttempts = 3;

bool isXbelDocument(const QString &contents) {
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

// Drops every earlier entry for the same file so the list keeps one bookmark
// per href, as GLib requires. Entries are found by their parsed href, so
// attribute order, quoting, and GLib's percent-encoding do not matter; only
// their own text is cut, keeping the rest of the file byte for byte.
void removeBookmarks(QString &contents, const QString &absolutePath) {
  QList<std::pair<qsizetype, qsizetype>> ranges;
  QXmlStreamReader reader(contents);
  qsizetype begin = -1;
  qint64 previousEnd = 0;
  while (!reader.atEnd()) {
    reader.readNext();
    if (reader.isStartElement() && reader.name() == QLatin1String("bookmark") &&
        QUrl::fromEncoded(reader.attributes()
                              .value(QLatin1String("href"))
                              .toUtf8())
                .toLocalFile() == absolutePath) {
      // The reader has already consumed the '<' when it reports the element.
      begin = contents.lastIndexOf(u'<', previousEnd);
    } else if (reader.isEndElement() && begin >= 0 &&
               reader.name() == QLatin1String("bookmark")) {
      ranges.append({begin, reader.characterOffset()});
      begin = -1;
    }
    previousEnd = reader.characterOffset();
  }
  if (reader.hasError())
    return;
  for (auto range = ranges.crbegin(); range != ranges.crend(); ++range) {
    qsizetype from = range->first;
    qsizetype to = range->second;
    while (from > 0 && contents.at(from - 1) == u' ')
      --from;
    if (to < contents.size() && contents.at(to) == u'\n')
      ++to;
    contents.remove(from, to - from);
  }
}

QString bookmarkEntry(const QString &escapedHref, const QString &mimeType) {
  const QString now =
      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  return QStringLiteral(
             "  <bookmark href=\"%1\" added=\"%2\" modified=\"%2\" visited=\"%2\">\n"
             "    <info>\n"
             "      <metadata owner=\"http://freedesktop.org\">\n"
             "        <mime:mime-type type=\"%3\"/>\n"
             "        <bookmark:applications>\n"
             "          <bookmark:application name=\"Omasnap\" "
             "exec=\"&apos;omasnap %u&apos;\" modified=\"%2\" count=\"1\"/>\n"
             "        </bookmark:applications>\n"
             "      </metadata>\n"
             "    </info>\n"
             "  </bookmark>\n")
      .arg(escapedHref, now, mimeType);
}

enum class ReadResult { Ok, Missing, Failed };

ReadResult readList(const QString &path, QByteArray &contents,
                    QString &error) {
  QFile file(path);
  if (!file.exists()) {
    contents.clear();
    return ReadResult::Missing;
  }
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read %1: %2").arg(path, file.errorString());
    return ReadResult::Failed;
  }
  contents = file.readAll();
  return ReadResult::Ok;
}

// The list with `entry` for `absolutePath` in place of any older one, or an
// empty string after setting `error`.
QString withBookmark(const QByteArray &existing, const QString &absolutePath,
                     const QString &entry, const QString &listPath,
                     QString &error) {
  QString contents = QString::fromUtf8(existing);
  // Editing as text must not rewrite anyone else's bytes.
  if (contents.toUtf8() != existing) {
    error = QStringLiteral("Recent files list is not UTF-8: %1").arg(listPath);
    return {};
  }
  if (contents.trimmed().isEmpty()) {
    contents = QLatin1String(kXbelHeader) + QLatin1String(kXbelFooter) + u'\n';
  } else if (!isXbelDocument(contents)) {
    error = QStringLiteral("Not an XBEL recent files list: %1").arg(listPath);
    return {};
  }
  removeBookmarks(contents, absolutePath);
  contents.insert(contents.lastIndexOf(QLatin1String(kXbelFooter)), entry);
  // A list with other namespace prefixes would no longer parse; keep it.
  if (!isXbelDocument(contents)) {
    error = QStringLiteral("Could not add an entry to %1").arg(listPath);
    return {};
  }
  return contents;
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
  const QString absolutePath = file.absoluteFilePath();
  const QString entry = bookmarkEntry(
      QString::fromUtf8(QUrl::fromLocalFile(absolutePath).toEncoded())
          .toHtmlEscaped(),
      QMimeDatabase().mimeTypeForFile(file).name().toHtmlEscaped());
  const QString listPath = desktopRecentFilesPath();
  if (!QDir().mkpath(QFileInfo(listPath).absolutePath())) {
    error = QStringLiteral("Could not create %1").arg(listPath);
    return false;
  }

  // Saves run on pool threads and in several Omasnap processes (a pin's
  // Show in folder while another capture saves). The mutex orders this
  // process's threads; an flock on a file in the private runtime directory
  // orders the processes.
  static QMutex mutex;
  const QMutexLocker locker(&mutex);
  const QString runtime = secureRuntimeDirectory();
  if (runtime.isEmpty()) {
    error = QStringLiteral("Could not create private runtime directory");
    return false;
  }
  // flock blocks until the holder finishes, without polling, and the kernel
  // drops it if that process dies, so a crash never leaves a stale lock.
  QFile lock(QDir(runtime).filePath(QStringLiteral("recently-used.lock")));
  if (!lock.open(QIODevice::ReadWrite) || ::flock(lock.handle(), LOCK_EX) != 0) {
    error = QStringLiteral("Could not lock %1: %2").arg(listPath, lock.errorString());
    return false;
  }

  // GTK applications write the list without any lock. Re-read it just before
  // replacing it, and start over when one of them changed it meanwhile, so
  // their additions are not overwritten.
  for (int attempt = 0; attempt < kWriteAttempts; ++attempt) {
    QByteArray before;
    if (readList(listPath, before, error) == ReadResult::Failed)
      return false;
    const QString contents =
        withBookmark(before, absolutePath, entry, listPath, error);
    if (contents.isEmpty())
      return false;
    const QByteArray bytes = contents.toUtf8();

    QSaveFile out(listPath);
    if (!out.open(QIODevice::WriteOnly) ||
        !out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        out.write(bytes) != bytes.size()) {
      error = QStringLiteral("Could not write %1: %2")
                  .arg(listPath, out.errorString());
      return false;
    }
    QByteArray current;
    if (readList(listPath, current, error) == ReadResult::Failed) {
      out.cancelWriting();
      return false;
    }
    if (current != before) {
      out.cancelWriting();
      continue;
    }
    if (!out.commit()) {
      error = QStringLiteral("Could not write %1: %2")
                  .arg(listPath, out.errorString());
      return false;
    }
    return true;
  }
  error = QStringLiteral("%1 kept changing while adding an entry").arg(listPath);
  return false;
}
