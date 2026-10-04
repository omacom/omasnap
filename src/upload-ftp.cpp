/** @fileoverview SFTP, FTP and FTPS uploads through curl. */
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include "upload-http.hpp"
#include "upload-provider.hpp"
#include "upload-sigv4.hpp"

namespace upload {

namespace {

const QString kSftp = QStringLiteral("SFTP");
const QString kFtp = QStringLiteral("FTP");
const QString kExplicitTls = QStringLiteral("FTPS (explicit TLS)");
const QString kImplicitTls = QStringLiteral("FTPS (implicit TLS)");
// curl gives up when less than a byte a second moves for this long.
constexpr int kStallSeconds = 60;

// A value for curl's config file: quoted, with \ and " escaped.
QByteArray configValue(const QString &value) {
  QByteArray bytes = value.toUtf8();
  bytes.replace('\\', "\\\\").replace('"', "\\\"").replace('\n', "\\n");
  return '"' + bytes + '"';
}

// Uploads through curl, which speaks FTP, FTPS and SFTP; Qt doesn't.
class CurlJob : public Job {
public:
  CurlJob(const QVariantMap &settings, QObject *parent)
      : Job(parent), settings_(settings),
        hostName_(settings.value(QStringLiteral("name")).toString()) {}

  ~CurlJob() override {
    if (process_) {
      process_->disconnect(this);
      process_->kill();
      process_->waitForFinished(1000);
    }
  }

  void start(const QString &filePath) override {
    const QString curl = QStandardPaths::findExecutable(QStringLiteral("curl"));
    if (curl.isEmpty()) {
      done(QStringLiteral(
          "`curl` was not found on your PATH; it's needed for FTP and SFTP."));
      return;
    }
    const QFileInfo file(filePath);
    if (!file.isReadable()) {
      done(QStringLiteral("Could not read %1.").arg(file.fileName()));
      return;
    }
    size_ = file.size();

    const QString fileName =
        settings_.value(QStringLiteral("uniqueNames"), true).toBool()
            ? taggedFileName(file.fileName())
            : file.fileName();
    const QString protocol =
        settings_.value(QStringLiteral("protocol")).toString();
    const QString host =
        settings_.value(QStringLiteral("host")).toString().trimmed();
    const int port = settings_.value(QStringLiteral("port")).toInt();
    QString directory =
        expandDateTokens(
            settings_.value(QStringLiteral("directory")).toString(),
            QDateTime::currentDateTime())
            .trimmed();
    while (directory.endsWith(QLatin1Char('/')))
      directory.chop(1);
    if (!directory.startsWith(QLatin1Char('/')))
      directory.prepend(QLatin1Char('/'));
    const QString remotePath =
        (directory == QStringLiteral("/") ? QString() : directory) +
        QLatin1Char('/') + fileName;

    const QString scheme = protocol == kSftp          ? QStringLiteral("sftp")
                           : protocol == kImplicitTls ? QStringLiteral("ftps")
                                                      : QStringLiteral("ftp");
    // SFTP paths are absolute from the root; "/~/" would mean home.
    const QString url =
        scheme + QStringLiteral("://") + host +
        (port > 0 ? QLatin1Char(':') + QString::number(port) : QString()) +
        QString::fromLatin1(sigv4::uriEncode(remotePath, false));

    // Everything, the password included, goes in over stdin so it
    // never appears in the process list.
    QByteArray config;
    config += "url = " + configValue(url) + '\n';
    config += "upload-file = " + configValue(filePath) + '\n';
    config +=
        "user = " +
        configValue(settings_.value(QStringLiteral("username")).toString() +
                    QLatin1Char(':') +
                    settings_.value(QStringLiteral("password")).toString()) +
        '\n';
    config += "ftp-create-dirs\nshow-error\nprogress-bar\n";
    config +=
        "speed-limit = 1\nspeed-time = " + QByteArray::number(kStallSeconds) +
        '\n';
    if (protocol == kExplicitTls)
      config += "ssl-reqd\n";
    const QString key =
        settings_.value(QStringLiteral("privateKey")).toString().trimmed();
    if (protocol == kSftp && !key.isEmpty())
      config +=
          "key = " + configValue(QDir::fromNativeSeparators(expandHome(key))) +
          '\n';

    QString publicUrl =
        settings_.value(QStringLiteral("publicUrl")).toString().trimmed();
    while (publicUrl.endsWith(QLatin1Char('/')))
      publicUrl.chop(1);
    link_ = publicUrl.isEmpty()
                ? url
                : publicUrl + QLatin1Char('/') +
                      QString::fromLatin1(sigv4::uriEncode(fileName, true));

    process_ = new QProcess(this);
    connect(process_, &QProcess::readyReadStandardError, this,
            [this] { readProgress(); });
    connect(
        process_, &QProcess::finished, this,
        [this](int code, QProcess::ExitStatus status) {
          readProgress();
          const QString error = curlError();
          process_->deleteLater();
          process_ = nullptr;
          if (cancelled_)
            done(QStringLiteral("Upload cancelled."));
          else if (status != QProcess::NormalExit || code != 0)
            done(QStringLiteral("%1: %2").arg(
                hostName_, error.isEmpty()
                               ? QStringLiteral("curl failed (%1)").arg(code)
                               : error));
          else
            done({}, Outcome{link_, {}, {}});
        });
    connect(process_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
              if (error != QProcess::FailedToStart)
                return;
              const QString message = process_->errorString();
              process_->deleteLater();
              process_ = nullptr;
              done(QStringLiteral("Could not start curl: %1").arg(message));
            });
    process_->start(curl, {QStringLiteral("--config"), QStringLiteral("-")});
    process_->write(config);
    process_->closeWriteChannel();
  }

  void cancel() override {
    if (finished_ || cancelled_)
      return;
    cancelled_ = true;
    if (process_)
      process_->kill();
    else
      done(QStringLiteral("Upload cancelled."));
  }

private:
  static QString expandHome(const QString &path) {
    return path.startsWith(QStringLiteral("~/"))
               ? QDir::homePath() + path.mid(1)
               : path;
  }

  // --progress-bar redraws "#####   42.5%" with carriage returns.
  void readProgress() {
    if (!process_)
      return;
    stderr_ += process_->readAllStandardError();
    static const QRegularExpression percent(
        QStringLiteral("(\\d+(?:\\.\\d+)?)%"));
    QRegularExpressionMatchIterator it =
        percent.globalMatch(QString::fromUtf8(stderr_.right(64)));
    double last = -1;
    while (it.hasNext())
      last = it.next().captured(1).toDouble();
    if (last >= 0 && size_ > 0)
      emit progress(qint64(size_ * qMin(last, 100.0) / 100.0), size_);
  }

  // curl's own "curl: (67) Access denied: 530" line, without the prefix.
  QString curlError() const {
    const QStringList lines = QString::fromUtf8(stderr_).split(
        QRegularExpression(QStringLiteral("[\\r\\n]")), Qt::SkipEmptyParts);
    for (qsizetype i = lines.size() - 1; i >= 0; --i) {
      const QString line = lines.at(i).trimmed();
      if (line.startsWith(QStringLiteral("curl: ")))
        return line.mid(6).remove(
            QRegularExpression(QStringLiteral("^\\(\\d+\\) ")));
    }
    return {};
  }

  void done(const QString &error, const Outcome &outcome = {}) {
    if (finished_)
      return;
    finished_ = true;
    if (error.isEmpty())
      emit finished(outcome);
    else
      emit failed(error);
  }

  const QVariantMap settings_;
  const QString hostName_;
  QProcess *process_ = nullptr;
  QByteArray stderr_;
  QString link_;
  qint64 size_ = 0;
  bool cancelled_ = false;
  bool finished_ = false;
};

class FtpProvider : public Provider {
public:
  QString id() const override { return QStringLiteral("ftp"); }
  QString name() const override { return QStringLiteral("FTP / SFTP"); }
  QString description() const override {
    return QStringLiteral(
        "Your own server over SFTP, FTP or FTPS (uses curl).");
  }

  QList<Field> fields() const override {
    QList<Field> fields;
    Field f;

    f = {};
    f.key = QStringLiteral("protocol");
    f.label = QStringLiteral("Protocol");
    f.type = Field::Choice;
    f.choices = {kSftp, kExplicitTls, kImplicitTls, kFtp};
    f.defaultValue = kSftp;
    fields << f;

    f = {};
    f.key = QStringLiteral("host");
    f.label = QStringLiteral("Server");
    f.placeholder = QStringLiteral("example.com");
    f.required = true;
    f.help = QStringLiteral(
        "For SFTP the server must already be in ~/.ssh/known_hosts.");
    fields << f;

    f = {};
    f.key = QStringLiteral("port");
    f.label = QStringLiteral("Port (0 = the protocol's default)");
    f.type = Field::Number;
    f.defaultValue = 0;
    fields << f;

    f = {};
    f.key = QStringLiteral("username");
    f.label = QStringLiteral("User name");
    f.required = true;
    fields << f;

    f = {};
    f.key = QStringLiteral("password");
    f.label = QStringLiteral("Password");
    f.type = Field::Secret;
    f.help = QStringLiteral("Leave empty for SFTP with a key.");
    fields << f;

    f = {};
    f.key = QStringLiteral("privateKey");
    f.label = QStringLiteral("SFTP private key");
    f.placeholder = QStringLiteral("~/.ssh/id_ed25519");
    fields << f;

    f = {};
    f.key = QStringLiteral("directory");
    f.label = QStringLiteral("Folder");
    f.defaultValue = QStringLiteral("/omasnap");
    f.help =
        QStringLiteral("Created when missing. %y, %mo and %d become the date.");
    fields << f;

    f = {};
    f.key = QStringLiteral("publicUrl");
    f.label = QStringLiteral("Web address of the folder");
    f.placeholder = QStringLiteral("https://example.com/shots");
    f.help = QStringLiteral(
        "Links point here; without it they're the server's own address.");
    fields << f;

    f = {};
    f.key = QStringLiteral("uniqueNames");
    f.label = QStringLiteral("Add a random tag to file names");
    f.type = Field::Toggle;
    f.defaultValue = true;
    fields << f;

    return fields;
  }

  Job *createJob(const QVariantMap &settings, const Services &,
                 QObject *parent) const override {
    return new CurlJob(settings, parent);
  }
};

} // namespace

const Provider *ftpProvider() {
  static const FtpProvider provider;
  return &provider;
}

} // namespace upload
