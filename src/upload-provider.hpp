/** @fileoverview Upload providers: the kinds of host a capture can be sent
 *  to, the settings each needs, and the job that performs one upload. */
#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>

#include <functional>

class QNetworkAccessManager;

// The pieces every upload destination is built from: a Provider describes a
// kind of host (S3, Dropbox, …) and the settings it needs, and makes a Job
// for each upload to a configured host.
namespace upload {

// One setting a provider asks for. The settings screen is generated from
// these, and Secret values are kept in the keyring rather than hosts.json.
struct Field {
  enum Type {
    Text,
    Multiline,
    Secret,
    Number,
    Toggle,
    Choice,
    // An ordered pick of other hosts (Auto).
    Hosts,
  };

  QString key;
  QString label;
  Type type = Text;
  QVariant defaultValue;
  QStringList choices;
  QString placeholder;
  QString help;
  bool required = false;
  // Kept but never shown, e.g. a token a sign-in stored.
  bool hidden = false;

  QVariantMap toVariant() const;
};

// Where an upload ended up.
struct Outcome {
  QString url;
  QString thumbnailUrl;
  QString deletionUrl;
};

class Job;

// What a job may lean on besides its own settings.
struct Services {
  QNetworkAccessManager *network = nullptr;
  // Stores changed settings of the host being used, secrets included —
  // e.g. a refreshed access token.
  std::function<void(const QVariantMap &changes)> saveSettings;
  // Opens a sign-in page in the browser.
  std::function<bool(const QUrl &url)> openUrl;
};

// One upload. start() leads to exactly one of finished or failed, and cancel()
// turns whatever is running into a failure with "Upload cancelled.".
class Job : public QObject {
  Q_OBJECT

public:
  using QObject::QObject;

  virtual void start(const QString &filePath) = 0;
  virtual void cancel() = 0;

signals:
  void progress(qint64 sent, qint64 total);
  void finished(const upload::Outcome &outcome);
  void failed(const QString &message);
};

// A sign-in that runs in the browser. Exactly one of finished or failed
// follows; finished carries the settings to store (tokens are Secret fields).
class Authorization : public QObject {
  Q_OBJECT

public:
  using QObject::QObject;
  virtual void start() = 0;
  virtual void cancel() = 0;

signals:
  // A line for the settings screen while waiting ("Waiting for Dropbox…").
  void status(const QString &message);
  void finished(const QVariantMap &changes, const QString &summary);
  void failed(const QString &message);
};

class Provider {
public:
  virtual ~Provider() = default;

  virtual QString id() const = 0;
  virtual QString name() const = 0;
  virtual QString description() const = 0;
  virtual QList<Field> fields() const = 0;

  // Why these settings can't upload, or empty. The default checks that
  // required fields are filled in.
  virtual QString validate(const QVariantMap &settings) const;

  virtual Job *createJob(const QVariantMap &settings, const Services &services,
                         QObject *parent) const = 0;

  // Providers that sign in through the browser instead of (or besides)
  // pasting a key.
  virtual QString authorizeLabel() const { return {}; }
  virtual Authorization *authorize(const QVariantMap &settings,
                                   const Services &services,
                                   QObject *parent) const;

  // settings with each field's default filled in where it's missing.
  QVariantMap withDefaults(const QVariantMap &settings) const;
};

// Every provider, by the `type` a [host.<name>] section names.
const QList<const Provider *> &providers();
const Provider *findProvider(const QString &id);

const Provider *s3Provider();
const Provider *dropboxProvider();
const Provider *nextcloudProvider();
const Provider *immichProvider();
const Provider *xbackboneProvider();
const Provider *ftpProvider();
const Provider *imgurProvider();
// A ShareX custom uploader, given as .sxcu JSON in the "definition" setting.
const Provider *sxcuProvider();

// The anonymous hosts offered out of the box, as .sxcu documents.
QList<QByteArray> builtInDefinitions();

} // namespace upload

Q_DECLARE_METATYPE(upload::Outcome)
