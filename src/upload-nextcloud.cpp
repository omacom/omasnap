/** @fileoverview Nextcloud WebDAV uploads, share links, and Login Flow v2
 * sign-in. */
#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QUrlQuery>

#include "upload-http.hpp"
#include "upload-provider.hpp"
#include "upload-sigv4.hpp"

namespace upload {

namespace {

// Nextcloud's login tokens last 20 minutes.
constexpr int kLoginTimeoutMs = 20 * 60 * 1000;
constexpr int kLoginPollMs = 2000;

QByteArray basicAuth(const QString &user, const QString &password) {
  return "Basic " + (user + QLatin1Char(':') + password).toUtf8().toBase64();
}

// Each segment encoded, the slashes between them kept.
QString encodePath(const QString &path) {
  return QString::fromLatin1(sigv4::uriEncode(path, false));
}

QStringList folderSegments(const QString &folder) {
  QStringList segments;
  for (const QString &segment :
       folder.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
    if (segment != QStringLiteral(".") && segment != QStringLiteral(".."))
      segments << segment;
  }
  return segments;
}

class NextcloudJob : public HttpJob {
public:
  NextcloudJob(const QVariantMap &settings, const Services &services,
               QObject *parent)
      : HttpJob(settings.value(QStringLiteral("name")).toString(), services,
                parent),
        settings_(settings),
        server_(normalizeServerUrl(
            settings.value(QStringLiteral("serverUrl")).toString())),
        user_(settings.value(QStringLiteral("username")).toString().trimmed()),
        auth_(basicAuth(
            user_, settings.value(QStringLiteral("appPassword")).toString())) {}

  void start(const QString &filePath) override {
    file_ = openFile(filePath);
    if (!file_)
      return;
    mimeType_ = mimeTypeFor(filePath);
    folders_ = folderSegments(
        expandDateTokens(settings_.value(QStringLiteral("folder")).toString(),
                         QDateTime::currentDateTime()));
    fileName_ = taggedFileName(QFileInfo(filePath).fileName());
    findUserId();
  }

private:
  QString davUrl(const QString &relativePath) const {
    return server_ + QStringLiteral("/remote.php/dav/files/") +
           QString::fromLatin1(sigv4::uriEncode(userId_, true)) +
           QLatin1Char('/') + encodePath(relativePath);
  }

  QString authError(const Reply &reply) const {
    return requestError(
        reply, reply.status == 401
                   ? QStringLiteral("check the user name and app password")
                   : QString());
  }

  // Files live under the user's id, which isn't always the name they
  // sign in with (e-mail logins, LDAP).
  void findUserId() {
    QNetworkRequest request =
        authed(server_ + QStringLiteral("/ocs/v1.php/cloud/user?format=json"));
    request.setRawHeader("OCS-APIRequest", "true");
    send(request, "GET", QByteArray(), [this](const Reply &reply) {
      userId_ = QJsonDocument::fromJson(reply.body)
                    .object()
                    .value(QStringLiteral("ocs"))
                    .toObject()
                    .value(QStringLiteral("data"))
                    .toObject()
                    .value(QStringLiteral("id"))
                    .toString();
      if (!reply.ok() || userId_.isEmpty()) {
        fail(authError(reply));
        return;
      }
      makeFolder(0);
    });
  }

  QNetworkRequest authed(const QString &url) const {
    QNetworkRequest request = newRequest(QUrl::fromEncoded(url.toUtf8()));
    request.setRawHeader("Authorization", auth_);
    return request;
  }

  // Creates each folder in turn; one that already exists answers 405.
  void makeFolder(int depth) {
    if (depth >= folders_.size()) {
      put();
      return;
    }
    const QString path = folders_.mid(0, depth + 1).join(QLatin1Char('/'));
    send(authed(davUrl(path)), "MKCOL", QByteArray(),
         [this, depth](const Reply &reply) {
           if (!reply.ok() && reply.status != 405) {
             fail(authError(reply));
             return;
           }
           makeFolder(depth + 1);
         });
  }

  QString relativeFilePath() const {
    return (folders_ + QStringList{fileName_}).join(QLatin1Char('/'));
  }

  void put() {
    QNetworkRequest request = authed(davUrl(relativeFilePath()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, mimeType_);
    send(
        request, "PUT", file_,
        [this](const Reply &reply) {
          if (!reply.ok()) {
            fail(requestError(reply));
            return;
          }
          share();
        },
        [this](qint64 sent, qint64 total) { emit progress(sent, total); });
    file_ = nullptr;
  }

  void share() {
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("path"),
                      QLatin1Char('/') + relativeFilePath());
    form.addQueryItem(QStringLiteral("shareType"), QStringLiteral("3"));
    form.addQueryItem(QStringLiteral("permissions"), QStringLiteral("1"));
    const int days = settings_.value(QStringLiteral("expireDays")).toInt();
    if (days > 0) {
      form.addQueryItem(
          QStringLiteral("expireDate"),
          QDate::currentDate().addDays(days).toString(Qt::ISODate));
    }

    QNetworkRequest request =
        authed(server_ +
               QStringLiteral(
                   "/ocs/v2.php/apps/files_sharing/api/v1/shares?format=json"));
    request.setRawHeader("OCS-APIRequest", "true");
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    send(request, "POST", form.toString(QUrl::FullyEncoded).toUtf8(),
         [this](const Reply &reply) {
           const QJsonObject ocs = QJsonDocument::fromJson(reply.body)
                                       .object()
                                       .value(QStringLiteral("ocs"))
                                       .toObject();
           QString url = ocs.value(QStringLiteral("data"))
                             .toObject()
                             .value(QStringLiteral("url"))
                             .toString();
           if (!reply.ok() || !isWebLink(url)) {
             const QString message = ocs.value(QStringLiteral("meta"))
                                         .toObject()
                                         .value(QStringLiteral("message"))
                                         .toString();
             fail(requestError(
                 reply,
                 message.isEmpty()
                     ? QStringLiteral("the file was uploaded but not shared")
                     : message));
             return;
           }
           if (settings_.value(QStringLiteral("directLink")).toBool())
             url += QStringLiteral("/download");
           succeed({url, {}, {}});
         });
  }

  const QVariantMap settings_;
  const QString server_;
  const QString user_;
  const QByteArray auth_;
  QString userId_;
  QIODevice *file_ = nullptr;
  QString mimeType_;
  QStringList folders_;
  QString fileName_;
};

// Nextcloud's Login Flow v2: the browser signs in and grants an app
// password, which is polled for here.
class NextcloudLogin : public Authorization {
public:
  NextcloudLogin(const QString &server, const Services &services,
                 QObject *parent)
      : Authorization(parent), server_(server), services_(services) {
    pollTimer_.setInterval(kLoginPollMs);
    connect(&pollTimer_, &QTimer::timeout, this, &NextcloudLogin::poll);
    deadline_.setSingleShot(true);
    deadline_.setInterval(kLoginTimeoutMs);
    connect(&deadline_, &QTimer::timeout, this, [this] {
      stop();
      emit failed(QStringLiteral("The Nextcloud sign-in timed out."));
    });
  }

  void start() override {
    if (server_.isEmpty()) {
      emit failed(QStringLiteral("Enter the server address first."));
      return;
    }
    QNetworkRequest request(
        QUrl(server_ + QStringLiteral("/index.php/login/v2")));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("omasnap"));
    reply_ = services_.network->post(request, QByteArray());
    connect(reply_, &QNetworkReply::finished, this, [this] {
      QNetworkReply *reply = reply_;
      reply_ = nullptr;
      reply->deleteLater();
      const QJsonObject body =
          QJsonDocument::fromJson(reply->readAll()).object();
      const QJsonObject pollInfo =
          body.value(QStringLiteral("poll")).toObject();
      token_ = pollInfo.value(QStringLiteral("token")).toString();
      endpoint_ = QUrl(pollInfo.value(QStringLiteral("endpoint")).toString());
      const QUrl login(body.value(QStringLiteral("login")).toString());
      if (reply->error() != QNetworkReply::NoError || token_.isEmpty() ||
          !login.isValid()) {
        emit failed(QStringLiteral("%1 didn't start a sign-in: %2")
                        .arg(server_, reply->errorString()));
        return;
      }
      services_.openUrl(login);
      emit status(
          QStringLiteral("Finish signing in to Nextcloud in your browser…"));
      deadline_.start();
      pollTimer_.start();
      poll();
    });
  }

  void cancel() override {
    stop();
    emit failed(QStringLiteral("Sign-in cancelled."));
  }

private:
  void stop() {
    pollTimer_.stop();
    deadline_.stop();
    if (reply_) {
      reply_->disconnect(this);
      reply_->abort();
      reply_->deleteLater();
      reply_ = nullptr;
    }
  }

  void poll() {
    if (reply_)
      return;
    QNetworkRequest request(endpoint_);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    reply_ = services_.network->post(
        request, "token=" + QUrl::toPercentEncoding(token_));
    connect(reply_, &QNetworkReply::finished, this, [this] {
      QNetworkReply *reply = reply_;
      reply_ = nullptr;
      reply->deleteLater();
      // 404 until the user has granted access.
      if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() !=
          200)
        return;
      const QJsonObject body =
          QJsonDocument::fromJson(reply->readAll()).object();
      const QString user = body.value(QStringLiteral("loginName")).toString();
      const QString password =
          body.value(QStringLiteral("appPassword")).toString();
      if (user.isEmpty() || password.isEmpty())
        return;
      stop();
      QVariantMap changes{{QStringLiteral("username"), user},
                          {QStringLiteral("appPassword"), password}};
      const QString server = body.value(QStringLiteral("server")).toString();
      if (!server.isEmpty())
        changes.insert(QStringLiteral("serverUrl"), normalizeServerUrl(server));
      emit finished(changes, QStringLiteral("Signed in as %1").arg(user));
    });
  }

  const QString server_;
  Services services_;
  QNetworkReply *reply_ = nullptr;
  QString token_;
  QUrl endpoint_;
  QTimer pollTimer_;
  QTimer deadline_;
};

class NextcloudProvider : public Provider {
public:
  QString id() const override { return QStringLiteral("nextcloud"); }
  QString name() const override { return QStringLiteral("Nextcloud"); }
  QString description() const override {
    return QStringLiteral("Upload to your Nextcloud and share a public link.");
  }

  QList<Field> fields() const override {
    QList<Field> fields;
    Field f;

    f = {};
    f.key = QStringLiteral("serverUrl");
    f.label = QStringLiteral("Server");
    f.placeholder = QStringLiteral("https://cloud.example.com");
    f.required = true;
    fields << f;

    f = {};
    f.key = QStringLiteral("username");
    f.label = QStringLiteral("User name");
    f.required = true;
    f.help = QStringLiteral(
        "Or save the server and use \"Sign in with Nextcloud\".");
    fields << f;

    f = {};
    f.key = QStringLiteral("appPassword");
    f.label = QStringLiteral("App password");
    f.type = Field::Secret;
    f.required = true;
    f.help =
        QStringLiteral("Create one under Settings › Security in Nextcloud.");
    fields << f;

    f = {};
    f.key = QStringLiteral("folder");
    f.label = QStringLiteral("Folder");
    f.defaultValue = QStringLiteral("omasnap/%y-%mo");
    fields << f;

    f = {};
    f.key = QStringLiteral("expireDays");
    f.label = QStringLiteral("Links expire after (days, 0 = never)");
    f.type = Field::Number;
    f.defaultValue = 0;
    fields << f;

    f = {};
    f.key = QStringLiteral("directLink");
    f.label = QStringLiteral("Link straight to the image file");
    f.type = Field::Toggle;
    f.defaultValue = false;
    fields << f;

    return fields;
  }

  Job *createJob(const QVariantMap &settings, const Services &services,
                 QObject *parent) const override {
    return new NextcloudJob(settings, services, parent);
  }

  QString authorizeLabel() const override {
    return QStringLiteral("Sign in with Nextcloud");
  }

  Authorization *authorize(const QVariantMap &settings,
                           const Services &services,
                           QObject *parent) const override {
    return new NextcloudLogin(
        normalizeServerUrl(
            settings.value(QStringLiteral("serverUrl")).toString()),
        services, parent);
  }
};

} // namespace

const Provider *nextcloudProvider() {
  static const NextcloudProvider provider;
  return &provider;
}

} // namespace upload
