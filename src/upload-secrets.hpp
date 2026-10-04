/** @fileoverview Where upload hosts keep passwords, keys and tokens. */
#pragma once

#include <QHash>
#include <QString>

#include <memory>

namespace upload {

// Where hosts keep their passwords, keys and tokens, apart from the
// readable settings in hosts.json.
class SecretStore {
public:
  virtual ~SecretStore() = default;

  // An empty string when nothing is stored.
  virtual QString read(const QString &key) const = 0;
  virtual bool write(const QString &key, const QString &value) = 0;
  virtual void remove(const QString &key) = 0;

  // The desktop keyring through libsecret's secret-tool when it works,
  // otherwise a file only the user can read, in dir. In QStandardPaths test
  // mode it's always the file, so tests never touch the real keyring.
  static std::unique_ptr<SecretStore> system(const QString &dir);
};

// A JSON file readable only by its owner.
class FileSecretStore : public SecretStore {
public:
  explicit FileSecretStore(QString path);

  QString read(const QString &key) const override;
  bool write(const QString &key, const QString &value) override;
  void remove(const QString &key) override;

private:
  QHash<QString, QString> load() const;
  bool save(const QHash<QString, QString> &secrets) const;

  QString path_;
};

// For tests.
class MemorySecretStore : public SecretStore {
public:
  QString read(const QString &key) const override {
    return secrets_.value(key);
  }
  bool write(const QString &key, const QString &value) override {
    secrets_.insert(key, value);
    return true;
  }
  void remove(const QString &key) override { secrets_.remove(key); }

private:
  QHash<QString, QString> secrets_;
};

} // namespace upload
