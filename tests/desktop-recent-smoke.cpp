/** @fileoverview Tests recently-used.xbel: create, append, replace, refuse. */
#include "desktop-recent-smoke.hpp"

#include "desktop-recent.hpp"

#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUrl>
#include <QXmlStreamReader>

namespace {
bool writeFile(const QString &path, const QByteArray &contents) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         file.write(contents) == contents.size();
}

QByteArray readFile(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Every bookmark href in document order, or an error for a malformed list.
bool bookmarkHrefs(const QByteArray &contents, QStringList &hrefs,
                   QString &error) {
  QXmlStreamReader reader(contents);
  while (!reader.atEnd()) {
    reader.readNext();
    if (reader.isStartElement() && reader.name() == QLatin1String("bookmark"))
      hrefs.append(reader.attributes().value(QLatin1String("href")).toString());
  }
  if (reader.hasError()) {
    error = QStringLiteral("recent list does not parse: %1")
                .arg(reader.errorString());
    return false;
  }
  return true;
}

QString href(const QString &path) {
  return QString::fromUtf8(QUrl::fromLocalFile(path).toEncoded());
}
} // namespace

bool runDesktopRecentSmoke(QString &error) {
  QTemporaryDir data;
  QTemporaryDir files;
  if (!data.isValid() || !files.isValid()) {
    error = QStringLiteral("could not create temporary directories");
    return false;
  }
  const QByteArray oldDataHome = qgetenv("XDG_DATA_HOME");
  const auto restore = qScopeGuard([&] {
    oldDataHome.isNull() ? qunsetenv("XDG_DATA_HOME")
                         : qputenv("XDG_DATA_HOME", oldDataHome);
  });
  qputenv("XDG_DATA_HOME", data.path().toUtf8());
  const QString list = desktopRecentFilesPath();
  if (list != data.filePath(QStringLiteral("recently-used.xbel"))) {
    error = QStringLiteral("recent list path ignores XDG_DATA_HOME: %1").arg(list);
    return false;
  }

  const QString first = files.filePath(QStringLiteral("screenshot-a.png"));
  const QString second = files.filePath(QStringLiteral("shot b & c.png"));
  if (!writeFile(first, "png") || !writeFile(second, "png")) {
    error = QStringLiteral("could not write fixture files");
    return false;
  }

  // A missing list is created with one entry.
  if (!addToDesktopRecentFiles(first, error))
    return false;
  QStringList hrefs;
  if (!bookmarkHrefs(readFile(list), hrefs, error))
    return false;
  if (hrefs != QStringList{href(first)}) {
    error = QStringLiteral("new recent list holds %1").arg(hrefs.join(u' '));
    return false;
  }
  if (!readFile(list).contains("<mime:mime-type type=\"image/png\"/>")) {
    error = QStringLiteral("recent entry has no PNG mime type");
    return false;
  }
  if (QFile(list).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) {
    error = QStringLiteral("recent list is readable by other users");
    return false;
  }

  // A second file appends, with spaces and & escaped; re-adding the first
  // moves it to the end instead of duplicating it.
  if (!addToDesktopRecentFiles(second, error) ||
      !addToDesktopRecentFiles(first, error))
    return false;
  hrefs.clear();
  if (!bookmarkHrefs(readFile(list), hrefs, error))
    return false;
  if (hrefs != QStringList{href(second), href(first)}) {
    error = QStringLiteral("re-added recent list holds %1").arg(hrefs.join(u' '));
    return false;
  }

  // Entries another application wrote survive untouched.
  const QByteArray foreign =
      "  <bookmark href=\"file:///tmp/other.txt\" added=\"2026-01-01T00:00:00Z\" "
      "modified=\"2026-01-01T00:00:00Z\" visited=\"2026-01-01T00:00:00Z\">\n"
      "    <info>\n"
      "      <metadata owner=\"http://freedesktop.org\">\n"
      "        <mime:mime-type type=\"text/plain\"/>\n"
      "        <bookmark:groups>\n"
      "          <bookmark:group>gedit</bookmark:group>\n"
      "        </bookmark:groups>\n"
      "        <bookmark:applications>\n"
      "          <bookmark:application name=\"gedit\" exec=\"&apos;gedit %u&apos;\" "
      "modified=\"2026-01-01T00:00:00Z\" count=\"3\"/>\n"
      "        </bookmark:applications>\n"
      "      </metadata>\n"
      "    </info>\n"
      "  </bookmark>\n";
  QByteArray contents = readFile(list);
  contents.insert(contents.indexOf("  <bookmark"), foreign);
  if (!writeFile(list, contents) || !addToDesktopRecentFiles(second, error))
    return false;
  contents = readFile(list);
  hrefs.clear();
  if (!bookmarkHrefs(contents, hrefs, error))
    return false;
  if (!contents.contains(foreign) ||
      hrefs != QStringList{QStringLiteral("file:///tmp/other.txt"), href(first),
                           href(second)}) {
    error = QStringLiteral("foreign entry not preserved: %1").arg(hrefs.join(u' '));
    return false;
  }

  // A list that is not XBEL is left alone.
  const QByteArray garbage = "not xml at all <";
  if (!writeFile(list, garbage)) {
    error = QStringLiteral("could not write garbage list");
    return false;
  }
  QString refused;
  if (addToDesktopRecentFiles(first, refused) || readFile(list) != garbage) {
    error = QStringLiteral("a malformed recent list was overwritten");
    return false;
  }

  // A file that does not exist is never listed.
  QFile::remove(list);
  if (addToDesktopRecentFiles(files.filePath(QStringLiteral("gone.png")), refused) ||
      QFile::exists(list)) {
    error = QStringLiteral("a missing file was added to recent files");
    return false;
  }
  return true;
}
