/** @fileoverview Secret storage through secret-tool, with a private-file
 * fallback. */
#include "upload-secrets.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>

namespace upload {

namespace {

// Long enough for the keyring to ask for its password.
constexpr int kKeyringTimeoutMs = 60000;
const QString kService = QStringLiteral("omasnap");

// secret-tool, with the file as a fallback: writes that the keyring refuses
// (no daemon, locked and dismissed) still land somewhere, and reads look in
// both so nothing stored either way goes missing.
class KeyringSecretStore : public SecretStore {
public:
  KeyringSecretStore(QString tool, QString fallbackPath)
      : tool_(std::move(tool)), fallback_(std::move(fallbackPath)) {}

  QString read(const QString &key) const override {
    QProcess proc;
    proc.start(tool_, {QStringLiteral("lookup"), QStringLiteral("service"),
                       kService, QStringLiteral("key"), key});
    if (proc.waitForFinished(kKeyringTimeoutMs) &&
        proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
      const QString value = QString::fromUtf8(proc.readAllStandardOutput());
      if (!value.isEmpty())
        return value;
    }
    return fallback_.read(key);
  }

  bool write(const QString &key, const QString &value) override {
    QProcess proc;
    proc.start(tool_, {QStringLiteral("store"),
                       QStringLiteral("--label=omasnap: %1").arg(key),
                       QStringLiteral("service"), kService,
                       QStringLiteral("key"), key});
    // The secret goes over stdin so it never shows up in a process list.
    proc.write(value.toUtf8());
    proc.closeWriteChannel();
    if (proc.waitForFinished(kKeyringTimeoutMs) &&
        proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
      fallback_.remove(key);
      return true;
    }
    return fallback_.write(key, value);
  }

  void remove(const QString &key) override {
    QProcess proc;
    proc.start(tool_, {QStringLiteral("clear"), QStringLiteral("service"),
                       kService, QStringLiteral("key"), key});
    proc.waitForFinished(kKeyringTimeoutMs);
    fallback_.remove(key);
  }

private:
  QString tool_;
  FileSecretStore fallback_;
};

} // namespace

std::unique_ptr<SecretStore> SecretStore::system(const QString &dir) {
  const QString filePath = QDir(dir).filePath(QStringLiteral("secrets.json"));
  const QString tool =
      QStandardPaths::findExecutable(QStringLiteral("secret-tool"));
  if (tool.isEmpty() || QStandardPaths::isTestModeEnabled())
    return std::make_unique<FileSecretStore>(filePath);
  return std::make_unique<KeyringSecretStore>(tool, filePath);
}

FileSecretStore::FileSecretStore(QString path) : path_(std::move(path)) {}

QHash<QString, QString> FileSecretStore::load() const {
  QHash<QString, QString> secrets;
  QFile file(path_);
  if (!file.open(QIODevice::ReadOnly))
    return secrets;
  const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
  for (auto it = object.begin(); it != object.end(); ++it)
    secrets.insert(it.key(), it.value().toString());
  return secrets;
}

bool FileSecretStore::save(const QHash<QString, QString> &secrets) const {
  if (secrets.isEmpty()) {
    QFile::remove(path_);
    return true;
  }
  QJsonObject object;
  for (auto it = secrets.begin(); it != secrets.end(); ++it)
    object.insert(it.key(), it.value());

  QDir().mkpath(QFileInfo(path_).path());
  QSaveFile file(path_);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  file.write(QJsonDocument(object).toJson());
  return file.commit();
}

QString FileSecretStore::read(const QString &key) const {
  return load().value(key);
}

bool FileSecretStore::write(const QString &key, const QString &value) {
  QHash<QString, QString> secrets = load();
  secrets.insert(key, value);
  return save(secrets);
}

void FileSecretStore::remove(const QString &key) {
  QHash<QString, QString> secrets = load();
  if (secrets.remove(key))
    save(secrets);
}

} // namespace upload
