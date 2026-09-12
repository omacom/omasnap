/** @fileoverview Screenshot destination and filename pattern from the user's
 *  INI config. */
#include "output-config.hpp"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

OutputConfig loadOutputConfig(const QString &filePath) {
  OutputConfig config;
  QSettings settings(filePath, QSettings::IniFormat);
  QString directory =
      settings.value(QStringLiteral("output/directory")).toString().trimmed();
  if (directory == QStringLiteral("~"))
    directory = QDir::homePath();
  else if (directory.startsWith(QStringLiteral("~/")))
    directory = QDir::homePath() + directory.mid(1);
  if (!directory.isEmpty())
    config.directory = directory;
  const QString filename =
      settings.value(QStringLiteral("output/filename")).toString().trimmed();
  if (!filename.isEmpty())
    config.filename = filename;
  config.logicalSize =
      settings.value(QStringLiteral("output/logical_size")).toString()
          .trimmed().compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
  return config;
}

QString formatScreenshotFilename(const QString &pattern, const QDateTime &when,
                                 const QString &appSlug) {
  QString name = pattern;
  if (appSlug.isEmpty()) {
    // Drop the token and the separator that introduced it, so the default
    // pattern does not leave a dangling dash on a capture with no app.
    for (const char *joined : {"-{app}", "_{app}", " {app}", "{app}-",
                               "{app}_", "{app} ", "{app}"})
      name.replace(QLatin1String(joined), QString());
  } else {
    name.replace(QStringLiteral("{app}"), appSlug);
  }
  name.replace(QStringLiteral("{date}"),
               when.toString(QStringLiteral("yyyy-MM-dd")));
  name.replace(QStringLiteral("{time}"),
               when.toString(QStringLiteral("HH-mm-ss")));
  // Filenames only: a slash would silently change the destination, and a
  // leading dot hides the file.
  name.replace(QLatin1Char('/'), QLatin1Char('-'));
  while (!name.isEmpty() &&
         QStringLiteral(". -_").contains(name.front()))
    name.remove(0, 1);
  name = name.trimmed();
  if (name.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
    name.chop(4);
  if (name.isEmpty())
    name = QStringLiteral("screenshot-") +
           when.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
  return name + QStringLiteral(".png");
}

QString defaultConfigPath() {
  return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
         QStringLiteral("/omasnap/omasnap.conf");
}
