/** @fileoverview Upload hosts from omasnap.conf: naming, settings, commands,
 *  the secret store, fallback between hosts, history, and sign-ins. */
#include <QtTest>

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QUrlQuery>

#include "upload-fake-server.hpp"
#include "upload-secrets.hpp"
#include "upload-test-kit.hpp"
#include "upload.hpp"

namespace {

// Receives the pages sign-ins would open in the browser.
class UrlCatcher : public QObject {
  Q_OBJECT

public:
  QList<QUrl> urls;

public slots:
  void open(const QUrl &url) { urls << url; }
};

} // namespace

class UploadConfigTests : public QObject {
  Q_OBJECT

private slots:
  void init();
  void uploadsAreOffUntilHostsAreNamed();
  void builtInHostsResolveByName();
  void sxcuFilesAreHostsNamedAfterTheFile();
  void hostSectionsMapConfigKeysToSettings();
  void commandsAndTheSecretStoreFillSecrets();
  void configMistakesAreExplained();
  void theReadmeExamplesResolve();
  void uploadFallsBackToTheNextHost();
  void uploadReportsEveryFailure();
  void uploadCanBeCancelled();
  void signInStoresADropboxToken();
  void signInStoresNextcloudCredentials();

private:
  void writeConfig(const QByteArray &contents);
  void writeSxcu(const QString &name, const QString &json);
  QString writeImage();
  QStringList history() const;

  std::unique_ptr<QTemporaryDir> dir_;
  UploadPaths paths_;
};

void UploadConfigTests::init() {
  dir_ = std::make_unique<QTemporaryDir>();
  QVERIFY(dir_->isValid());
  paths_ = {dir_->filePath(QStringLiteral("omasnap.conf")),
            dir_->filePath(QStringLiteral("uploaders")),
            dir_->filePath(QStringLiteral("data")),
            dir_->filePath(QStringLiteral("state/uploads.jsonl"))};
}

void UploadConfigTests::writeConfig(const QByteArray &contents) {
  QFile file(paths_.configPath);
  QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write(contents);
}

void UploadConfigTests::writeSxcu(const QString &name, const QString &json) {
  QVERIFY(QDir().mkpath(paths_.uploadersDir));
  QFile file(
      QDir(paths_.uploadersDir).filePath(name + QStringLiteral(".sxcu")));
  QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write(json.toUtf8());
}

QString UploadConfigTests::writeImage() {
  const QString path = dir_->filePath(QStringLiteral("screenshot.png"));
  QImage image(4, 4, QImage::Format_RGB32);
  image.fill(Qt::red);
  if (!image.save(path))
    return {};
  return path;
}

QStringList UploadConfigTests::history() const {
  QFile file(paths_.historyPath);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return QString::fromUtf8(file.readAll())
      .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

static QString sxcuTo(const FakeHttpServer &server) {
  sxcu::Destination destination;
  destination.requestUrl = server.url();
  destination.fileFormName = QStringLiteral("file");
  return sxcuJson(destination);
}

void UploadConfigTests::uploadsAreOffUntilHostsAreNamed() {
  QVERIFY(!uploadConfigured(paths_.configPath));
  writeConfig("[output]\nautosave=true\n[upload]\nhosts=\n");
  QVERIFY(!uploadConfigured(paths_.configPath));
  QString error;
  QVERIFY(loadUploadHosts(paths_, error).isEmpty());
  QCOMPARE(error, QStringLiteral(
                      "Uploads are off: set [upload] hosts in omasnap.conf"));

  writeConfig("[upload]\nhosts=litterbox\n");
  QVERIFY(uploadConfigured(paths_.configPath));
}

void UploadConfigTests::builtInHostsResolveByName() {
  writeConfig("[upload]\nhosts = Catbox, litterbox, uguu\n");
  QString error;
  const QList<UploadHost> hosts = loadUploadHosts(paths_, error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QCOMPARE(hosts.size(), 3);
  QCOMPARE(hosts.at(0).name, QStringLiteral("Catbox"));
  QCOMPARE(hosts.at(0).provider, QStringLiteral("sxcu"));
  QVERIFY(hosts.at(0)
              .settings.value(QStringLiteral("definition"))
              .toString()
              .contains(QStringLiteral("catbox.moe")));
  QCOMPARE(hosts.at(1).settings.value(QStringLiteral("name")).toString(),
           QStringLiteral("Litterbox (72 hours)"));
}

void UploadConfigTests::sxcuFilesAreHostsNamedAfterTheFile() {
  writeSxcu(QStringLiteral("mine"),
            QString::fromUtf8(
                R"({"RequestURL": "https://x.example", "FileFormName": "f"})"));
  writeSxcu(QStringLiteral("broken"), QStringLiteral("{"));
  writeConfig("[upload]\nhosts = mine\n");
  QString error;
  const QList<UploadHost> hosts = loadUploadHosts(paths_, error);
  QCOMPARE(hosts.size(), 1);
  QCOMPARE(hosts.first().provider, QStringLiteral("sxcu"));
  QCOMPARE(hosts.first().settings.value(QStringLiteral("name")).toString(),
           QStringLiteral("mine"));

  writeConfig("[upload]\nhosts = mine, broken\n");
  QVERIFY(loadUploadHosts(paths_, error).isEmpty());
  QVERIFY2(error.startsWith(QStringLiteral("broken.sxcu can't be used: ")),
           qPrintable(error));
}

void UploadConfigTests::hostSectionsMapConfigKeysToSettings() {
  writeConfig("[upload]\nhosts = bucket\n"
              "[host.bucket]\n"
              "type = s3\n"
              "bucket = shots\n"
              "access_key_id = AKID\n"
              "secret_access_key = plain\n"
              "path_style = yes\n"
              "multipart_threshold = 1234\n"
              "link_type = signed\n"
              "storage_class = standard_ia\n");
  QString error;
  const QList<UploadHost> hosts = loadUploadHosts(paths_, error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  const QVariantMap settings = hosts.first().settings;
  QCOMPARE(hosts.first().provider, QStringLiteral("s3"));
  QCOMPARE(settings.value(QStringLiteral("bucket")).toString(),
           QStringLiteral("shots"));
  QCOMPARE(settings.value(QStringLiteral("accessKeyId")).toString(),
           QStringLiteral("AKID"));
  QCOMPARE(settings.value(QStringLiteral("pathStyle")).toBool(), true);
  QCOMPARE(settings.value(QStringLiteral("multipartThreshold")).toLongLong(),
           1234);
  // Choices match forgivingly.
  QCOMPARE(settings.value(QStringLiteral("linkType")).toString(),
           QStringLiteral("Signed link (7 days)"));
  QCOMPARE(settings.value(QStringLiteral("storageClass")).toString(),
           QStringLiteral("STANDARD_IA"));
  // Unset keys take the provider's defaults.
  QCOMPARE(settings.value(QStringLiteral("region")).toString(),
           QStringLiteral("us-east-1"));
  QCOMPARE(settings.value(QStringLiteral("name")).toString(),
           QStringLiteral("bucket"));
}

void UploadConfigTests::commandsAndTheSecretStoreFillSecrets() {
  writeConfig("[upload]\nhosts = cloud, box\n"
              "[host.cloud]\n"
              "type = xbackbone\n"
              "server_url = https://x.example\n"
              "token_command = printf 'from-command\\n'\n"
              "[host.box]\n"
              "type = immich\n"
              "server_url = https://photos.example\n");
  // What --sign-in (or `secret-tool store`) left behind.
  const std::unique_ptr<upload::SecretStore> secrets =
      upload::SecretStore::system(paths_.secretsDir);
  QVERIFY(secrets->write(QStringLiteral("box/api_key"),
                         QStringLiteral("from-store")));

  QString error;
  const QList<UploadHost> hosts = loadUploadHosts(paths_, error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QCOMPARE(hosts.at(0).settings.value(QStringLiteral("token")).toString(),
           QStringLiteral("from-command"));
  QCOMPARE(hosts.at(1).settings.value(QStringLiteral("apiKey")).toString(),
           QStringLiteral("from-store"));
}

void UploadConfigTests::configMistakesAreExplained() {
  const auto errorFor = [this](const QByteArray &config) {
    writeConfig(config);
    QString error;
    const QList<UploadHost> hosts = loadUploadHosts(paths_, error);
    return hosts.isEmpty() ? error : QStringLiteral("(no error)");
  };

  QVERIFY(errorFor("[upload]\nhosts = nowhere\n")
              .startsWith(
                  QStringLiteral("No host named nowhere: add [host.nowhere]")));
  QVERIFY(
      errorFor("[upload]\nhosts = x\n[host.x]\ntype = gopher\n")
          .startsWith(QStringLiteral("[host.x] needs type = s3 | dropbox")));
  QCOMPARE(
      errorFor("[upload]\nhosts = x\n[host.x]\ntype = s3\naccess_key_id = a\n"
               "secret_access_key = b\n"),
      QStringLiteral("[host.x] needs bucket (or bucket_command)"));
  QVERIFY(
      errorFor(
          "[upload]\nhosts = x\n[host.x]\ntype = ftp\nhost = h\nusername = u\n"
          "protocol = gopher\n")
          .startsWith(
              QStringLiteral("[host.x] protocol takes one of: SFTP, ")));
  QVERIFY(errorFor("[upload]\nhosts = x\n[host.x]\ntype = imgur\n"
                   "client_id_command = \"echo nope >&2; exit 3\"\n")
              .startsWith(
                  QStringLiteral("[host.x] client_id_command failed: nope")));
  // Signing in is suggested where that's what's missing.
  QCOMPARE(
      errorFor(
          "[upload]\nhosts = box\n[host.box]\ntype = dropbox\napp_key = k\n"),
      QStringLiteral("[host.box] Sign in with Dropbox first. Run: omasnap "
                     "--sign-in box"));
}

// The keys and values the README shows, so the docs can't drift from the
// field names.
void UploadConfigTests::theReadmeExamplesResolve() {
  writeConfig("[upload]\n"
              "hosts = work, cloud, box, photos, server, xbb\n"
              "[host.work]\n"
              "type = s3\n"
              "endpoint = acc.r2.cloudflarestorage.com\n"
              "region = auto\n"
              "bucket = shots\n"
              "access_key_id = AKIA\n"
              "secret_access_key = x\n"
              "object_prefix = omasnap/%y/%mo\n"
              "custom_domain = https://cdn.example.com\n"
              "link_type = signed\n"
              "path_style = true\n"
              "public_acl = true\n"
              "storage_class = standard_ia\n"
              "unique_names = false\n"
              "[host.cloud]\n"
              "type = nextcloud\n"
              "server_url = https://cloud.example.com\n"
              "username = u\n"
              "app_password = p\n"
              "folder = omasnap/%y-%mo\n"
              "expire_days = 7\n"
              "direct_link = true\n"
              "[host.box]\n"
              "type = dropbox\n"
              "app_key = abc\n"
              "refresh_token = t\n"
              "folder = /omasnap\n"
              "direct_link = true\n"
              "[host.photos]\n"
              "type = immich\n"
              "server_url = https://photos.example.com\n"
              "api_key = k\n"
              "share_link = false\n"
              "expire_days = 7\n"
              "public_url = https://share.example.com\n"
              "[host.server]\n"
              "type = ftp\n"
              "protocol = ftps-implicit\n"
              "host = example.com\n"
              "username = me\n"
              "private_key = ~/.ssh/id_ed25519\n"
              "directory = /var/www/shots\n"
              "public_url = https://example.com/shots\n"
              "[host.xbb]\n"
              "type = xbackbone\n"
              "server_url = https://x.example\n"
              "token = t\n"
              "api = v1\n");
  QString error;
  const QList<UploadHost> hosts = loadUploadHosts(paths_, error);
  QVERIFY2(hosts.size() == 6, qPrintable(error));
  const auto setting = [&hosts](int host, const char *key) {
    return hosts.at(host).settings.value(QString::fromLatin1(key));
  };
  QCOMPARE(setting(0, "objectPrefix").toString(),
           QStringLiteral("omasnap/%y/%mo"));
  QCOMPARE(setting(0, "linkType").toString(),
           QStringLiteral("Signed link (7 days)"));
  QCOMPARE(setting(0, "uniqueNames").toBool(), false);
  QCOMPARE(setting(1, "expireDays").toInt(), 7);
  QCOMPARE(setting(1, "directLink").toBool(), true);
  QCOMPARE(setting(2, "folder").toString(), QStringLiteral("/omasnap"));
  QCOMPARE(setting(3, "shareLink").toBool(), false);
  QCOMPARE(setting(4, "protocol").toString(),
           QStringLiteral("FTPS (implicit TLS)"));
  QCOMPARE(setting(4, "privateKey").toString(),
           QStringLiteral("~/.ssh/id_ed25519"));
  QCOMPARE(setting(5, "api").toString(),
           QStringLiteral("API v1 (/api/v1/upload)"));
}

void UploadConfigTests::uploadFallsBackToTheNextHost() {
  FakeHttpServer broken;
  QVERIFY(broken.listen());
  broken.status = 503;
  broken.reply = "down";
  FakeHttpServer working;
  QVERIFY(working.listen());
  working.reply = "https://files.example/shot.png";
  writeSxcu(QStringLiteral("broken"), sxcuTo(broken));
  writeSxcu(QStringLiteral("working"), sxcuTo(working));
  writeConfig("[upload]\nhosts = broken, working\n");

  QList<int> progress;
  const UploadResult result = uploadFile(
      writeImage(), paths_, [&progress](int percent) { progress << percent; });
  QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
  QCOMPARE(result.url, QStringLiteral("https://files.example/shot.png"));
  QCOMPARE(result.host, QStringLiteral("working"));
  QCOMPARE(broken.requests.size(), 1);
  QCOMPARE(working.requests.size(), 1);
  QVERIFY(!progress.isEmpty());
  QCOMPARE(progress.last(), 100);

  const QStringList lines = history();
  QCOMPARE(lines.size(), 1);
  const QJsonObject entry =
      QJsonDocument::fromJson(lines.first().toUtf8()).object();
  QCOMPARE(entry.value(QStringLiteral("host")).toString(),
           QStringLiteral("working"));
  QCOMPARE(entry.value(QStringLiteral("url")).toString(),
           QStringLiteral("https://files.example/shot.png"));
}

void UploadConfigTests::uploadReportsEveryFailure() {
  FakeHttpServer broken;
  QVERIFY(broken.listen());
  broken.status = 500;
  broken.reply = "boom";
  writeSxcu(QStringLiteral("one"), sxcuTo(broken));
  writeSxcu(QStringLiteral("two"), sxcuTo(broken));
  writeConfig("[upload]\nhosts = one, two\n");

  const UploadResult result = uploadFile(writeImage(), paths_);
  QCOMPARE(result.error, QStringLiteral("one answered HTTP 500: boom; "
                                        "two answered HTTP 500: boom"));
  QVERIFY(result.url.isEmpty());
  QVERIFY(history().isEmpty());
}

void UploadConfigTests::uploadCanBeCancelled() {
  QTcpServer silent;
  QVERIFY(silent.listen(QHostAddress::LocalHost));
  sxcu::Destination destination;
  destination.requestUrl =
      QStringLiteral("http://127.0.0.1:%1/").arg(silent.serverPort());
  destination.fileFormName = QStringLiteral("f");
  writeSxcu(QStringLiteral("silent"), sxcuJson(destination));
  writeConfig("[upload]\nhosts = silent, litterbox\n");

  // Cancelled once the host has the connection; litterbox is never tried.
  std::atomic_bool cancel = false;
  QTimer poll;
  connect(&poll, &QTimer::timeout, &poll, [&] {
    if (silent.hasPendingConnections())
      cancel = true;
  });
  poll.start(10);
  const UploadResult result = uploadFile(writeImage(), paths_, {}, &cancel);
  QCOMPARE(result.error, QStringLiteral("Upload cancelled"));
}

void UploadConfigTests::signInStoresADropboxToken() {
  FakeHttpServer server;
  QVERIFY(server.listen());
  server.responder =
      [](const FakeHttpServer::Request &request) -> FakeHttpServer::Response {
    if (request.target == "/oauth2/token")
      return {200, R"({"access_token":"a","refresh_token":"refresh-new"})", {}};
    return {200, R"({"name":{"display_name":"Ada"}})", {}};
  };
  QTcpServer probe;
  QVERIFY(probe.listen(QHostAddress::LocalHost));
  const QString redirect = QStringLiteral("http://127.0.0.1:%1/oauth2/callback")
                               .arg(probe.serverPort());
  probe.close();
  writeConfig(
      "[upload]\nhosts = box\n[host.box]\ntype = dropbox\napp_key = app-key\n"
      "redirect_uri = " +
      redirect.toUtf8() + "\nbase_url = " + server.url(QString()).toUtf8() +
      "\n");

  // Play the browser: once the sign-in page is "opened", come back to the
  // redirect with a code.
  UrlCatcher browser;
  QDesktopServices::setUrlHandler(QStringLiteral("http"), &browser, "open");
  QNetworkAccessManager network;
  QTimer callback;
  QNetworkReply *reply = nullptr;
  connect(&callback, &QTimer::timeout, &callback, [&] {
    if (browser.urls.isEmpty() || reply)
      return;
    const QString state =
        QUrlQuery(browser.urls.first()).queryItemValue(QStringLiteral("state"));
    reply = network.get(QNetworkRequest(
        QUrl(redirect + QStringLiteral("?code=the-code&state=") + state)));
  });
  callback.start(10);
  const int status = runUploadSignIn(QStringLiteral("box"), paths_);
  QDesktopServices::unsetUrlHandler(QStringLiteral("http"));
  QCOMPARE(status, 0);
  QVERIFY(reply);
  reply->deleteLater();

  const QUrlQuery asked(browser.urls.first());
  QCOMPARE(asked.queryItemValue(QStringLiteral("client_id")),
           QStringLiteral("app-key"));
  const QByteArray verifier =
      QUrlQuery(QString::fromUtf8(server.requests.at(0).body))
          .queryItemValue(QStringLiteral("code_verifier"))
          .toLatin1();
  QCOMPARE(QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
               .toBase64(QByteArray::Base64UrlEncoding |
                         QByteArray::OmitTrailingEquals),
           asked.queryItemValue(QStringLiteral("code_challenge")).toLatin1());

  // The token is in the secret store, not the config, and the host now works.
  QCOMPARE(upload::SecretStore::system(paths_.secretsDir)
               ->read(QStringLiteral("box/refresh_token")),
           QStringLiteral("refresh-new"));
  QFile config(paths_.configPath);
  QVERIFY(config.open(QIODevice::ReadOnly));
  QVERIFY(!config.readAll().contains("refresh-new"));
  QString error;
  QCOMPARE(loadUploadHosts(paths_, error).size(), 1);
  QVERIFY2(error.isEmpty(), qPrintable(error));
}

void UploadConfigTests::signInStoresNextcloudCredentials() {
  FakeHttpServer server;
  QVERIFY(server.listen());
  server.responder =
      [&server](
          const FakeHttpServer::Request &request) -> FakeHttpServer::Response {
    if (request.target == "/index.php/login/v2") {
      return {200,
              R"({"poll":{"token":"tok","endpoint":")" +
                  server.url(QStringLiteral("/login/v2/poll")).toLatin1() +
                  R"("},"login":")" +
                  server.url(QStringLiteral("/login/v2/flow/x")).toLatin1() +
                  R"("})",
              {}};
    }
    return {200, R"({"loginName":"alice","appPassword":"granted"})", {}};
  };
  writeConfig(
      "[upload]\nhosts = cloud\n[host.cloud]\ntype = nextcloud\nserver_url = " +
      server.url(QString()).toUtf8() + "\n");

  // Without the sign-in, the host isn't usable yet.
  QString error;
  QVERIFY(loadUploadHosts(paths_, error).isEmpty());
  QCOMPARE(error,
           QStringLiteral("[host.cloud] needs username (or username_command)"));

  UrlCatcher browser;
  QDesktopServices::setUrlHandler(QStringLiteral("http"), &browser, "open");
  const int status = runUploadSignIn(QStringLiteral("cloud"), paths_);
  QDesktopServices::unsetUrlHandler(QStringLiteral("http"));
  QCOMPARE(status, 0);
  QCOMPARE(browser.urls,
           QList<QUrl>{QUrl(server.url(QStringLiteral("/login/v2/flow/x")))});

  const QList<UploadHost> hosts = loadUploadHosts(paths_, error);
  QVERIFY2(hosts.size() == 1, qPrintable(error));
  QCOMPARE(hosts.first().settings.value(QStringLiteral("username")).toString(),
           QStringLiteral("alice"));
  QCOMPARE(
      hosts.first().settings.value(QStringLiteral("appPassword")).toString(),
      QStringLiteral("granted"));
}

int runUploadConfigSmoke(int argc, char **argv) {
  UploadConfigTests tests;
  return QTest::qExec(&tests, argc, argv);
}

#include "upload-config-smoke.moc"
