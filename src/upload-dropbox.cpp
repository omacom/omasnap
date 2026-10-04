/** @fileoverview Dropbox uploads, shared links, and the PKCE browser sign-in.
 */
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>

#include "upload-http.hpp"
#include "upload-provider.hpp"

namespace upload {

namespace {

const QString kDefaultRedirect =
    QStringLiteral("http://127.0.0.1:52475/oauth2/callback");
// Dropbox takes up to 150 MB per request; bigger files go in sessions.
constexpr qint64 kSingleUploadLimit = 150LL * 1024 * 1024;
constexpr qint64 kChunkSize = 32LL * 1024 * 1024;
constexpr int kSignInTimeoutMs = 5 * 60 * 1000;

struct Endpoints {
  QString web = QStringLiteral("https://www.dropbox.com");
  QString api = QStringLiteral("https://api.dropboxapi.com");
  QString content = QStringLiteral("https://content.dropboxapi.com");
};

// Tests point everything at one local server.
Endpoints endpointsFor(const QVariantMap &settings) {
  Endpoints endpoints;
  const QString base = settings.value(QStringLiteral("baseUrl")).toString();
  if (!base.isEmpty())
    endpoints = {base, base, base};
  return endpoints;
}

// Dropbox-API-Arg is an HTTP header, so anything outside ASCII is escaped.
QByteArray asciiJson(const QJsonObject &object) {
  const QString json =
      QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
  QByteArray out;
  for (const QChar c : json) {
    if (c.unicode() < 0x80)
      out += char(c.unicode());
    else
      out += QStringLiteral("\\u%1")
                 .arg(uint(c.unicode()), 4, 16, QLatin1Char('0'))
                 .toLatin1();
  }
  return out;
}

QString errorSummary(const QByteArray &body) {
  const QJsonObject object = QJsonDocument::fromJson(body).object();
  QString summary = object.value(QStringLiteral("error_summary")).toString();
  if (summary.isEmpty())
    summary = object.value(QStringLiteral("error_description")).toString();
  // "path/insufficient_space/.." -> "path/insufficient_space"
  while (summary.endsWith(QLatin1Char('.')) ||
         summary.endsWith(QLatin1Char('/')))
    summary.chop(1);
  return summary;
}

QString randomToken(int length) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
  QString token;
  for (int i = 0; i < length; ++i)
    token += QLatin1Char(alphabet[QRandomGenerator::system()->bounded(66)]);
  return token;
}

class DropboxJob : public HttpJob {
public:
  DropboxJob(const QVariantMap &settings, const Services &services,
             QObject *parent)
      : HttpJob(settings.value(QStringLiteral("name")).toString(), services,
                parent),
        settings_(settings), endpoints_(endpointsFor(settings)) {
    // Tests shrink both the single-request limit and the chunks.
    const qint64 chunkSize =
        settings.value(QStringLiteral("chunkSize")).toLongLong();
    if (chunkSize > 0) {
      chunkSize_ = chunkSize;
      singleLimit_ = chunkSize;
    }
  }

  void start(const QString &filePath) override {
    file_ = openFile(filePath);
    if (!file_)
      return;
    size_ = file_->size();

    QString folder =
        expandDateTokens(settings_.value(QStringLiteral("folder")).toString(),
                         QDateTime::currentDateTime())
            .trimmed();
    while (folder.endsWith(QLatin1Char('/')))
      folder.chop(1);
    if (!folder.startsWith(QLatin1Char('/')))
      folder.prepend(QLatin1Char('/'));
    path_ = (folder == QStringLiteral("/") ? QString() : folder) +
            QLatin1Char('/') + QFileInfo(filePath).fileName();
    refreshToken();
  }

private:
  QNetworkRequest apiRequest(const QString &url,
                             const QByteArray &contentType) const {
    QNetworkRequest request = newRequest(QUrl(url));
    request.setRawHeader("Authorization", "Bearer " + accessToken_.toUtf8());
    request.setRawHeader("Content-Type", contentType);
    return request;
  }

  QString failure(const Reply &reply) const {
    return requestError(reply, errorSummary(reply.body));
  }

  // Access tokens last hours; the stored refresh token gets a fresh one.
  void refreshToken() {
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("grant_type"),
                      QStringLiteral("refresh_token"));
    form.addQueryItem(
        QStringLiteral("refresh_token"),
        settings_.value(QStringLiteral("refreshToken")).toString());
    form.addQueryItem(
        QStringLiteral("client_id"),
        settings_.value(QStringLiteral("appKey")).toString().trimmed());
    QNetworkRequest request =
        newRequest(QUrl(endpoints_.api + QStringLiteral("/oauth2/token")));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    send(request, "POST", form.toString(QUrl::FullyEncoded).toUtf8(),
         [this](const Reply &reply) {
           accessToken_ = QJsonDocument::fromJson(reply.body)
                              .object()
                              .value(QStringLiteral("access_token"))
                              .toString();
           if (!reply.ok() || accessToken_.isEmpty()) {
             fail(reply.status == 400 || reply.status == 401
                      ? QStringLiteral("%1 needs signing in again.")
                            .arg(hostName_)
                      : failure(reply));
             return;
           }
           if (size_ > singleLimit_)
             startSession();
           else
             uploadWhole();
         });
  }

  QJsonObject commitInfo() const {
    return {{QStringLiteral("path"), path_},
            {QStringLiteral("mode"), QStringLiteral("add")},
            {QStringLiteral("autorename"), true},
            {QStringLiteral("mute"), true}};
  }

  Progress progressFrom(qint64 offset) {
    return [this, offset](qint64 sent, qint64 total) {
      if (total > 0)
        emit progress(offset + sent, size_);
    };
  }

  void uploadWhole() {
    QNetworkRequest request =
        apiRequest(endpoints_.content + QStringLiteral("/2/files/upload"),
                   "application/octet-stream");
    request.setRawHeader("Dropbox-API-Arg", asciiJson(commitInfo()));
    send(
        request, "POST", file_, [this](const Reply &reply) { uploaded(reply); },
        progressFrom(0));
    file_ = nullptr;
  }

  void startSession() {
    QNetworkRequest request = apiRequest(
        endpoints_.content + QStringLiteral("/2/files/upload_session/start"),
        "application/octet-stream");
    request.setRawHeader("Dropbox-API-Arg",
                         asciiJson({{QStringLiteral("close"), false}}));
    const QByteArray chunk = file_->read(chunkSize_);
    send(
        request, "POST", chunk,
        [this, length = chunk.size()](const Reply &reply) {
          sessionId_ = QJsonDocument::fromJson(reply.body)
                           .object()
                           .value(QStringLiteral("session_id"))
                           .toString();
          if (!reply.ok() || sessionId_.isEmpty()) {
            fail(failure(reply));
            return;
          }
          appendChunk(length);
        },
        progressFrom(0));
  }

  // Sends the chunk at offset, or finishes the session with the last one.
  void appendChunk(qint64 offset) {
    const QByteArray chunk = file_->read(chunkSize_);
    const bool last = offset + chunk.size() >= size_;
    const QJsonObject cursor{{QStringLiteral("session_id"), sessionId_},
                             {QStringLiteral("offset"), offset}};
    QNetworkRequest request = apiRequest(
        endpoints_.content +
            (last ? QStringLiteral("/2/files/upload_session/finish")
                  : QStringLiteral("/2/files/upload_session/append_v2")),
        "application/octet-stream");
    request.setRawHeader(
        "Dropbox-API-Arg",
        asciiJson(last ? QJsonObject{{QStringLiteral("cursor"), cursor},
                                     {QStringLiteral("commit"), commitInfo()}}
                       : QJsonObject{{QStringLiteral("cursor"), cursor}}));
    send(
        request, "POST", chunk,
        [this, last, next = offset + chunk.size()](const Reply &reply) {
          if (last) {
            uploaded(reply);
            return;
          }
          if (!reply.ok()) {
            fail(failure(reply));
            return;
          }
          appendChunk(next);
        },
        progressFrom(offset));
  }

  void uploaded(const Reply &reply) {
    const QJsonObject metadata = QJsonDocument::fromJson(reply.body).object();
    // autorename may have changed the name.
    const QString path =
        metadata.value(QStringLiteral("path_lower")).toString();
    if (!reply.ok() || path.isEmpty()) {
      fail(failure(reply));
      return;
    }
    share(path);
  }

  void share(const QString &path) {
    QNetworkRequest request = apiRequest(
        endpoints_.api +
            QStringLiteral("/2/sharing/create_shared_link_with_settings"),
        "application/json");
    const QJsonObject body{{QStringLiteral("path"), path}};
    send(request, "POST", QJsonDocument(body).toJson(QJsonDocument::Compact),
         [this](const Reply &reply) {
           const QJsonObject object =
               QJsonDocument::fromJson(reply.body).object();
           QString url = object.value(QStringLiteral("url")).toString();
           // A second upload to the same path already has a link.
           if (url.isEmpty()) {
             url = object.value(QStringLiteral("error"))
                       .toObject()
                       .value(QStringLiteral("shared_link_already_exists"))
                       .toObject()
                       .value(QStringLiteral("metadata"))
                       .toObject()
                       .value(QStringLiteral("url"))
                       .toString();
           }
           if (!isWebLink(url)) {
             fail(failure(reply));
             return;
           }
           succeed({directLink(url), {}, {}});
         });
  }

  QString directLink(const QString &url) const {
    if (!settings_.value(QStringLiteral("directLink")).toBool())
      return url;
    QUrl link(url);
    QUrlQuery query(link);
    query.removeAllQueryItems(QStringLiteral("dl"));
    query.addQueryItem(QStringLiteral("raw"), QStringLiteral("1"));
    link.setQuery(query);
    return link.toString(QUrl::FullyEncoded);
  }

  const QVariantMap settings_;
  const Endpoints endpoints_;
  qint64 chunkSize_ = kChunkSize;
  qint64 singleLimit_ = kSingleUploadLimit;
  QIODevice *file_ = nullptr;
  qint64 size_ = 0;
  QString path_;
  QString accessToken_;
  QString sessionId_;
};

// OAuth 2 with PKCE: the browser signs in and Dropbox redirects back to a
// one-off listener on the loopback address with a code to trade for a
// refresh token. No client secret is involved.
class DropboxSignIn : public Authorization {
public:
  DropboxSignIn(const QVariantMap &settings, const Services &services,
                QObject *parent)
      : Authorization(parent), services_(services),
        endpoints_(endpointsFor(settings)),
        appKey_(settings.value(QStringLiteral("appKey")).toString().trimmed()),
        redirect_(
            settings.value(QStringLiteral("redirectUri"), kDefaultRedirect)
                .toString()) {
    timeout_.setSingleShot(true);
    timeout_.setInterval(kSignInTimeoutMs);
    connect(&timeout_, &QTimer::timeout, this, [this] {
      finish(QStringLiteral("The Dropbox sign-in timed out."));
    });
    connect(&listener_, &QTcpServer::newConnection, this,
            &DropboxSignIn::accept);
  }

  void start() override {
    if (appKey_.isEmpty()) {
      emit failed(QStringLiteral("Enter your Dropbox app key first."));
      return;
    }
    const QUrl redirect(redirect_);
    if (!listener_.listen(QHostAddress::LocalHost, quint16(redirect.port()))) {
      emit failed(QStringLiteral("Could not listen on %1 for the sign-in: %2")
                      .arg(redirect_, listener_.errorString()));
      return;
    }

    verifier_ = randomToken(64);
    state_ = randomToken(24);
    const QByteArray challenge =
        QCryptographicHash::hash(verifier_.toLatin1(),
                                 QCryptographicHash::Sha256)
            .toBase64(QByteArray::Base64UrlEncoding |
                      QByteArray::OmitTrailingEquals);

    QUrl authorize(endpoints_.web + QStringLiteral("/oauth2/authorize"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client_id"), appKey_);
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("token_access_type"),
                       QStringLiteral("offline"));
    query.addQueryItem(QStringLiteral("code_challenge"),
                       QString::fromLatin1(challenge));
    query.addQueryItem(QStringLiteral("code_challenge_method"),
                       QStringLiteral("S256"));
    query.addQueryItem(QStringLiteral("redirect_uri"), redirect_);
    query.addQueryItem(QStringLiteral("state"), state_);
    authorize.setQuery(query);

    timeout_.start();
    services_.openUrl(authorize);
    emit status(
        QStringLiteral("Finish signing in to Dropbox in your browser…"));
  }

  void cancel() override { finish(QStringLiteral("Sign-in cancelled.")); }

private:
  void accept() {
    while (QTcpSocket *socket = listener_.nextPendingConnection()) {
      connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
        QByteArray &buffer = buffers_[socket];
        buffer += socket->readAll();
        if (!buffer.contains("\r\n\r\n"))
          return;
        const QByteArray target =
            buffer.left(buffer.indexOf("\r\n")).split(' ').value(1);
        buffers_.remove(socket);
        handleRedirect(socket, QUrl::fromEncoded("http://localhost" + target));
      });
    }
  }

  void respond(QTcpSocket *socket, const QString &message) {
    const QByteArray html =
        "<!doctype html><meta charset=utf-8><title>omasnap</title>"
        "<body style=\"font:16px sans-serif;margin:3em\">" +
        message.toHtmlEscaped().toUtf8() + "</body>";
    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; "
                  "charset=utf-8\r\nContent-Length: " +
                  QByteArray::number(html.size()) +
                  "\r\nConnection: close\r\n\r\n" + html);
    socket->disconnectFromHost();
  }

  void handleRedirect(QTcpSocket *socket, const QUrl &url) {
    const QUrlQuery query(url);
    if (url.path() != QUrl(redirect_).path() || exchanging_) {
      respond(socket, QStringLiteral("Nothing to see here."));
      return;
    }
    if (query.queryItemValue(QStringLiteral("state")) != state_) {
      respond(socket,
              QStringLiteral(
                  "This sign-in doesn't match the one omasnap started."));
      return;
    }
    const QString code =
        query.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
    if (code.isEmpty()) {
      const QString error = query.queryItemValue(
          QStringLiteral("error_description"), QUrl::FullyDecoded);
      respond(socket, QStringLiteral(
                          "Dropbox wasn't connected. You can close this tab."));
      finish(error.isEmpty() ? QStringLiteral("Dropbox access wasn't granted.")
                             : QStringLiteral("Dropbox: %1").arg(error));
      return;
    }
    respond(socket, QStringLiteral("Dropbox is connected. You can close this "
                                   "tab and go back to omasnap."));
    listener_.close();
    exchange(code);
  }

  void exchange(const QString &code) {
    exchanging_ = true;
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("grant_type"),
                      QStringLiteral("authorization_code"));
    form.addQueryItem(QStringLiteral("code"), code);
    form.addQueryItem(QStringLiteral("client_id"), appKey_);
    form.addQueryItem(QStringLiteral("redirect_uri"), redirect_);
    form.addQueryItem(QStringLiteral("code_verifier"), verifier_);
    QNetworkRequest request(
        QUrl(endpoints_.api + QStringLiteral("/oauth2/token")));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    reply_ = services_.network->post(
        request, form.toString(QUrl::FullyEncoded).toUtf8());
    connect(reply_, &QNetworkReply::finished, this, [this] {
      QNetworkReply *reply = reply_;
      reply_ = nullptr;
      reply->deleteLater();
      const QByteArray body = reply->readAll();
      const QJsonObject token = QJsonDocument::fromJson(body).object();
      const QString refresh =
          token.value(QStringLiteral("refresh_token")).toString();
      if (refresh.isEmpty()) {
        const QString why = errorSummary(body);
        finish(QStringLiteral("Dropbox didn't grant access: %1")
                   .arg(why.isEmpty() ? reply->errorString() : why));
        return;
      }
      refreshToken_ = refresh;
      fetchAccount(token.value(QStringLiteral("access_token")).toString());
    });
  }

  // Only for the "Connected as …" line; failing it doesn't matter.
  void fetchAccount(const QString &accessToken) {
    QNetworkRequest request(
        QUrl(endpoints_.api + QStringLiteral("/2/users/get_current_account")));
    request.setRawHeader("Authorization", "Bearer " + accessToken.toUtf8());
    reply_ = services_.network->post(request, QByteArray());
    connect(reply_, &QNetworkReply::finished, this, [this] {
      QNetworkReply *reply = reply_;
      reply_ = nullptr;
      reply->deleteLater();
      const QString name = QJsonDocument::fromJson(reply->readAll())
                               .object()
                               .value(QStringLiteral("name"))
                               .toObject()
                               .value(QStringLiteral("display_name"))
                               .toString();
      timeout_.stop();
      done_ = true;
      emit finished({{QStringLiteral("refreshToken"), refreshToken_}},
                    name.isEmpty()
                        ? QStringLiteral("Connected to Dropbox")
                        : QStringLiteral("Connected as %1").arg(name));
    });
  }

  void finish(const QString &error) {
    if (done_)
      return;
    done_ = true;
    timeout_.stop();
    listener_.close();
    if (reply_) {
      reply_->disconnect(this);
      reply_->abort();
      reply_->deleteLater();
      reply_ = nullptr;
    }
    emit failed(error);
  }

  Services services_;
  const Endpoints endpoints_;
  const QString appKey_;
  const QString redirect_;
  QTcpServer listener_;
  QHash<QTcpSocket *, QByteArray> buffers_;
  QTimer timeout_;
  QNetworkReply *reply_ = nullptr;
  QString verifier_;
  QString state_;
  QString refreshToken_;
  bool exchanging_ = false;
  bool done_ = false;
};

class DropboxProvider : public Provider {
public:
  QString id() const override { return QStringLiteral("dropbox"); }
  QString name() const override { return QStringLiteral("Dropbox"); }
  QString description() const override {
    return QStringLiteral(
        "Save the capture to your Dropbox and share a link to it.");
  }

  QList<Field> fields() const override {
    QList<Field> fields;
    Field f;

    f = {};
    f.key = QStringLiteral("appKey");
    f.label = QStringLiteral("App key");
    f.required = true;
    f.help =
        QStringLiteral(
            "Create an app at dropbox.com/developers/apps with the "
            "files.content.write and sharing.write permissions, and add %1 "
            "as its redirect URI. Then sign in.")
            .arg(kDefaultRedirect);
    fields << f;

    f = {};
    f.key = QStringLiteral("folder");
    f.label = QStringLiteral("Folder");
    f.defaultValue = QStringLiteral("/omasnap");
    f.help = QStringLiteral(
        "Inside the app's folder when it has App folder access.");
    fields << f;

    f = {};
    f.key = QStringLiteral("directLink");
    f.label = QStringLiteral("Link straight to the image file");
    f.type = Field::Toggle;
    f.defaultValue = false;
    fields << f;

    f = {};
    f.key = QStringLiteral("refreshToken");
    f.label = QStringLiteral("Dropbox sign-in");
    f.type = Field::Secret;
    f.hidden = true;
    fields << f;

    for (const char *key : {"redirectUri", "baseUrl", "chunkSize"}) {
      f = {};
      f.key = QString::fromLatin1(key);
      f.label = f.key;
      f.hidden = true;
      fields << f;
    }
    return fields;
  }

  QString validate(const QVariantMap &settings) const override {
    const QString missing = Provider::validate(settings);
    if (!missing.isEmpty())
      return missing;
    if (settings.value(QStringLiteral("refreshToken")).toString().isEmpty())
      return QStringLiteral("Sign in with Dropbox first.");
    return {};
  }

  Job *createJob(const QVariantMap &settings, const Services &services,
                 QObject *parent) const override {
    return new DropboxJob(settings, services, parent);
  }

  QString authorizeLabel() const override {
    return QStringLiteral("Sign in with Dropbox");
  }

  Authorization *authorize(const QVariantMap &settings,
                           const Services &services,
                           QObject *parent) const override {
    return new DropboxSignIn(settings, services, parent);
  }
};

} // namespace

const Provider *dropboxProvider() {
  static const DropboxProvider provider;
  return &provider;
}

} // namespace upload
