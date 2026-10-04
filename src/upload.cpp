/** @fileoverview Configured upload hosts, the blocking upload runner, and the
 *  --upload / --sign-in command-line entry points. */
#include "upload.hpp"

#include "capture.hpp"
#include "output-config.hpp"
#include "upload-provider.hpp"
#include "upload-secrets.hpp"
#include "upload-sxcu.hpp"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cstdio>

namespace {

constexpr int kSecretCommandTimeoutMs = 30000;
const QString kHostPrefix = QStringLiteral("host.");

// accessKeyId -> access_key_id, the spelling config files use.
QString configKey(const QString &fieldKey) {
  QString key;
  for (const QChar c : fieldKey) {
    if (c.isUpper()) {
      key += QLatin1Char('_');
      key += c.toLower();
    } else {
      key += c;
    }
  }
  return key;
}

// Lowercase letters and digits only, for forgiving choice matching.
QString slug(const QString &text) {
  QString out;
  for (const QChar c : text.toLower()) {
    if (c.isLetterOrNumber())
      out += c;
  }
  return out;
}

// "signed" picks "Signed link (7 days)", "sftp" picks "SFTP": an exact match
// on letters and digits first, then the first choice containing them.
QString matchChoice(const QString &value, const QStringList &choices) {
  const QString wanted = slug(value);
  for (const QString &choice : choices) {
    if (slug(choice) == wanted)
      return choice;
  }
  for (const QString &choice : choices) {
    if (!wanted.isEmpty() && slug(choice).contains(wanted))
      return choice;
  }
  return {};
}

// QSettings reads an unquoted comma as a list separator; put the text back.
// Values with a ";" (a comment in INI) or "," are best written quoted.
QString configString(const QVariant &value) {
  return value.typeId() == QMetaType::QStringList
             ? value.toStringList().join(QLatin1Char(','))
             : value.toString();
}

bool parseBool(const QString &value) {
  const QString v = value.trimmed().toLower();
  return v == QStringLiteral("true") || v == QStringLiteral("yes") ||
         v == QStringLiteral("on") || v == QStringLiteral("1");
}

// Runs a `<key>_command` such as `secret-tool lookup ...` or `pass show ...`.
bool runSecretCommand(const QString &command, QString &value, QString &error) {
  QProcess process;
  process.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), command});
  if (!process.waitForFinished(kSecretCommandTimeoutMs)) {
    process.kill();
    process.waitForFinished(1000);
    error = QStringLiteral("timed out");
    return false;
  }
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (error.isEmpty())
      error = QStringLiteral("exited with %1").arg(process.exitCode());
    return false;
  }
  value = QString::fromUtf8(process.readAllStandardOutput());
  // Commands print a trailing newline that isn't part of the secret.
  while (value.endsWith(QLatin1Char('\n')) || value.endsWith(QLatin1Char('\r')))
    value.chop(1);
  return true;
}

QString secretKey(const QString &host, const QString &key) {
  return host + QLatin1Char('/') + key;
}

std::unique_ptr<upload::SecretStore> secretStore(const UploadPaths &paths) {
  return upload::SecretStore::system(paths.secretsDir);
}

// The settings a host's provider needs, from its config section, commands
// and the secret store. `validate` is off for sign-in, which is how missing
// secrets get filled in.
bool resolveHost(const UploadPaths &paths, const QString &name, bool validate,
                 UploadHost &host, QString &error) {
  host = {name, {}, {}};

  // A built-in anonymous host, by the first word of its name.
  for (const QByteArray &definition : upload::builtInDefinitions()) {
    sxcu::Destination destination;
    if (sxcu::fromJson(definition, &destination) &&
        destination.name.section(QLatin1Char(' '), 0, 0).toLower() ==
            name.toLower()) {
      host.provider = QStringLiteral("sxcu");
      host.settings = {
          {QStringLiteral("definition"), QString::fromUtf8(definition)},
          {QStringLiteral("name"), destination.name}};
      return true;
    }
  }

  // A .sxcu file named after the host.
  const QString sxcuPath =
      QDir(paths.uploadersDir).filePath(name + QStringLiteral(".sxcu"));
  if (QFileInfo::exists(sxcuPath)) {
    QFile file(sxcuPath);
    QString problem;
    if (!file.open(QIODevice::ReadOnly) ||
        !sxcu::fromJson(file.readAll(), nullptr, &problem)) {
      error = QStringLiteral("%1.sxcu can't be used: %2")
                  .arg(name, problem.isEmpty() ? file.errorString() : problem);
      return false;
    }
    file.seek(0);
    host.provider = QStringLiteral("sxcu");
    host.settings = {
        {QStringLiteral("definition"), QString::fromUtf8(file.readAll())},
        {QStringLiteral("name"), name}};
    return true;
  }

  // A [host.<name>] section.
  QSettings config(paths.configPath, QSettings::IniFormat);
  const QString group = kHostPrefix + name;
  if (!config.childGroups().contains(group)) {
    error = QStringLiteral(
                "No host named %1: add [%2] to omasnap.conf or %1.sxcu to %3")
                .arg(name, group, paths.uploadersDir);
    return false;
  }
  config.beginGroup(group);
  host.provider =
      config.value(QStringLiteral("type")).toString().trimmed().toLower();
  const upload::Provider *provider = upload::findProvider(host.provider);
  if (!provider || host.provider == QStringLiteral("sxcu")) {
    QStringList types;
    for (const upload::Provider *p : upload::providers()) {
      if (p->id() != QStringLiteral("sxcu"))
        types << p->id();
    }
    error = QStringLiteral("[%1] needs type = %2")
                .arg(group, types.join(QStringLiteral(" | ")));
    return false;
  }

  const std::unique_ptr<upload::SecretStore> secrets = secretStore(paths);
  for (const upload::Field &field : provider->fields()) {
    const QString key = configKey(field.key);
    QString raw;
    bool found = false;
    if (config.contains(key)) {
      raw = configString(config.value(key));
      found = true;
    } else if (config.contains(key + QStringLiteral("_command"))) {
      QString problem;
      if (!runSecretCommand(
              configString(config.value(key + QStringLiteral("_command"))), raw,
              problem)) {
        error = QStringLiteral("[%1] %2_command failed: %3")
                    .arg(group, key, problem);
        return false;
      }
      found = true;
    } else if (field.type == upload::Field::Secret || field.required) {
      // Where --sign-in leaves tokens, passwords and the user name.
      raw = secrets->read(secretKey(name, key));
      found = !raw.isEmpty();
    }
    if (!found)
      continue;

    switch (field.type) {
    case upload::Field::Toggle:
      host.settings.insert(field.key, parseBool(raw));
      break;
    case upload::Field::Number:
      host.settings.insert(field.key, raw.trimmed().toLongLong());
      break;
    case upload::Field::Choice: {
      const QString choice = matchChoice(raw, field.choices);
      if (choice.isEmpty()) {
        error = QStringLiteral("[%1] %2 takes one of: %3")
                    .arg(group, key, field.choices.join(QStringLiteral(", ")));
        return false;
      }
      host.settings.insert(field.key, choice);
      break;
    }
    default:
      host.settings.insert(field.key, raw.trimmed());
      break;
    }
  }
  host.settings = provider->withDefaults(host.settings);
  host.settings.insert(QStringLiteral("name"), name);

  if (!validate)
    return true;
  for (const upload::Field &field : provider->fields()) {
    if (field.required &&
        host.settings.value(field.key).toString().trimmed().isEmpty()) {
      error = QStringLiteral("[%1] needs %2 (or %2_command)")
                  .arg(group, configKey(field.key));
      return false;
    }
  }
  const QString problem = provider->validate(host.settings);
  if (!problem.isEmpty()) {
    error = provider->authorizeLabel().isEmpty()
                ? QStringLiteral("[%1] %2").arg(group, problem)
                : QStringLiteral("[%1] %2 Run: omasnap --sign-in %3")
                      .arg(group, problem, name);
    return false;
  }
  return true;
}

QStringList configuredHostNames(const QString &configPath) {
  QSettings config(configPath, QSettings::IniFormat);
  // QSettings splits an unquoted comma list itself.
  const QVariant value = config.value(QStringLiteral("upload/hosts"));
  QStringList names;
  const QStringList parts = value.typeId() == QMetaType::QStringList
                                ? value.toStringList()
                                : value.toString().split(QLatin1Char(','));
  for (const QString &part : parts) {
    const QString name = part.trimmed();
    if (!name.isEmpty())
      names << name;
  }
  return names;
}

// Stores what a sign-in or token refresh returned under the host's name.
void storeChanges(const UploadPaths &paths, const QString &host,
                  const QVariantMap &changes) {
  const std::unique_ptr<upload::SecretStore> secrets = secretStore(paths);
  for (auto it = changes.begin(); it != changes.end(); ++it)
    secrets->write(secretKey(host, configKey(it.key())), it.value().toString());
}

upload::Services servicesFor(const UploadPaths &paths, const QString &host,
                             QNetworkAccessManager *network) {
  upload::Services services;
  services.network = network;
  services.saveSettings = [paths, host](const QVariantMap &changes) {
    storeChanges(paths, host, changes);
  };
  services.openUrl = [](const QUrl &url) {
    return QDesktopServices::openUrl(url);
  };
  return services;
}

void appendHistory(const UploadPaths &paths, const QString &path,
                   const UploadResult &result) {
  QJsonObject entry{
      {QStringLiteral("time"),
       QDateTime::currentDateTime().toString(Qt::ISODate)},
      {QStringLiteral("host"), result.host},
      {QStringLiteral("file"), path},
      {QStringLiteral("url"), result.url},
  };
  if (!result.thumbnailUrl.isEmpty())
    entry.insert(QStringLiteral("thumbnail"), result.thumbnailUrl);
  if (!result.deletionUrl.isEmpty())
    entry.insert(QStringLiteral("deletion"), result.deletionUrl);
  QDir().mkpath(QFileInfo(paths.historyPath).path());
  QFile file(paths.historyPath);
  if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    file.write(QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n');
}

} // namespace

UploadPaths UploadPaths::defaults() {
  const QString config =
      QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
      QStringLiteral("/omasnap");
  return {
      defaultConfigPath(),
      config + QStringLiteral("/uploaders"),
      QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
          QStringLiteral("/omasnap"),
      QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) +
          QStringLiteral("/omasnap/uploads.jsonl"),
  };
}

bool uploadConfigured(const QString &configPath) {
  return !configuredHostNames(configPath).isEmpty();
}

QList<UploadHost> loadUploadHosts(const UploadPaths &paths, QString &error) {
  const QStringList names = configuredHostNames(paths.configPath);
  if (names.isEmpty()) {
    error =
        QStringLiteral("Uploads are off: set [upload] hosts in omasnap.conf");
    return {};
  }
  QList<UploadHost> hosts;
  for (const QString &name : names) {
    UploadHost host;
    if (!resolveHost(paths, name, true, host, error))
      return {};
    hosts << host;
  }
  return hosts;
}

UploadResult uploadFile(const QString &path, const UploadPaths &paths,
                        const std::function<void(int)> &progress,
                        const std::atomic_bool *cancel) {
  UploadResult result;
  const QList<UploadHost> hosts = loadUploadHosts(paths, result.error);
  if (hosts.isEmpty())
    return result;

  // This thread's own network stack and event loop; nothing here touches
  // the GUI thread.
  QNetworkAccessManager network;
  QStringList failures;
  for (const UploadHost &host : hosts) {
    if (cancel && cancel->load()) {
      result.error = QStringLiteral("Upload cancelled");
      return result;
    }
    const upload::Provider *provider = upload::findProvider(host.provider);
    QEventLoop loop;
    upload::Job *job = provider->createJob(
        host.settings, servicesFor(paths, host.name, &network), nullptr);
    bool succeeded = false;
    QString failure;
    QObject::connect(job, &upload::Job::progress, job,
                     [&progress](qint64 sent, qint64 total) {
                       if (progress && total > 0)
                         progress(static_cast<int>(
                             std::clamp<qint64>(sent * 100 / total, 0, 100)));
                     });
    QObject::connect(job, &upload::Job::finished, job,
                     [&](const upload::Outcome &outcome) {
                       succeeded = true;
                       result.url = outcome.url;
                       result.thumbnailUrl = outcome.thumbnailUrl;
                       result.deletionUrl = outcome.deletionUrl;
                       loop.quit();
                     });
    QObject::connect(job, &upload::Job::failed, job,
                     [&](const QString &message) {
                       failure = message;
                       loop.quit();
                     });
    QTimer cancelPoll;
    if (cancel) {
      QObject::connect(&cancelPoll, &QTimer::timeout, job, [job, cancel] {
        if (cancel->load())
          job->cancel();
      });
      cancelPoll.start(100);
    }
    QTimer::singleShot(0, job, [job, path] { job->start(path); });
    loop.exec();
    delete job;

    if (succeeded) {
      result.host = host.name;
      result.error.clear();
      appendHistory(paths, path, result);
      return result;
    }
    if (cancel && cancel->load()) {
      result.error = QStringLiteral("Upload cancelled");
      return result;
    }
    failures << failure;
  }
  result.error = failures.join(QStringLiteral("; "));
  return result;
}

int runUploadCommand(const QString &path, const UploadPaths &paths) {
  if (!QFileInfo(path).isFile()) {
    std::fprintf(stderr, "omasnap: %s is not a file\n", qPrintable(path));
    return 2;
  }
  const UploadResult result = uploadFile(path, paths);
  if (!result.error.isEmpty()) {
    std::fprintf(stderr, "omasnap: %s\n", qPrintable(result.error));
    return 1;
  }
  QString copyError;
  if (!copyTextToClipboard(result.url, copyError))
    std::fprintf(stderr, "omasnap: link not copied: %s\n",
                 qPrintable(copyError));
  std::printf("%s\n", qPrintable(result.url));
  if (!result.deletionUrl.isEmpty())
    std::fprintf(stderr, "delete: %s\n", qPrintable(result.deletionUrl));
  return 0;
}

int runUploadSignIn(const QString &hostName, const UploadPaths &paths) {
  UploadHost host;
  QString error;
  if (!resolveHost(paths, hostName, false, host, error)) {
    std::fprintf(stderr, "omasnap: %s\n", qPrintable(error));
    return 2;
  }
  const upload::Provider *provider = upload::findProvider(host.provider);
  QNetworkAccessManager network;
  upload::Authorization *authorization =
      provider
          ? provider->authorize(host.settings,
                                servicesFor(paths, hostName, &network), nullptr)
          : nullptr;
  if (!authorization) {
    std::fprintf(stderr,
                 "omasnap: %s doesn't sign in; set its keys in omasnap.conf\n",
                 qPrintable(hostName));
    return 2;
  }

  QEventLoop loop;
  int status = 1;
  QObject::connect(authorization, &upload::Authorization::status, authorization,
                   [](const QString &message) {
                     std::fprintf(stderr, "%s\n", qPrintable(message));
                   });
  QObject::connect(authorization, &upload::Authorization::finished,
                   authorization,
                   [&](const QVariantMap &changes, const QString &summary) {
                     storeChanges(paths, hostName, changes);
                     std::printf("%s\n", qPrintable(summary));
                     status = 0;
                     loop.quit();
                   });
  QObject::connect(authorization, &upload::Authorization::failed, authorization,
                   [&](const QString &message) {
                     std::fprintf(stderr, "omasnap: %s\n", qPrintable(message));
                     loop.quit();
                   });
  QTimer::singleShot(0, authorization,
                     [authorization] { authorization->start(); });
  loop.exec();
  delete authorization;
  return status;
}
