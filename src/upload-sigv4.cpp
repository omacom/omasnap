/** @fileoverview AWS Signature Version 4 signing and presigned URLs. */
#include "upload-sigv4.hpp"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QUrlQuery>

#include <algorithm>

namespace upload::sigv4 {

namespace {

const QByteArray kAlgorithm = QByteArrayLiteral("AWS4-HMAC-SHA256");

QByteArray hmac(const QByteArray &key, const QByteArray &data) {
  return QMessageAuthenticationCode::hash(data, key,
                                          QCryptographicHash::Sha256);
}

QByteArray sha256Hex(const QByteArray &data) {
  return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

QByteArray amzDate(const QDateTime &now) {
  return now.toUTC()
      .toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"))
      .toLatin1();
}

QByteArray dateStamp(const QDateTime &now) {
  return now.toUTC().toString(QStringLiteral("yyyyMMdd")).toLatin1();
}

QByteArray scope(const Credentials &credentials, const QDateTime &now) {
  return dateStamp(now) + '/' + credentials.region.toUtf8() + '/' +
         credentials.service.toUtf8() + "/aws4_request";
}

QByteArray hostHeader(const QUrl &url) {
  QByteArray host = url.host(QUrl::FullyEncoded).toLatin1();
  const int port = url.port();
  const bool defaultPort =
      port < 0 || (url.scheme() == QStringLiteral("https") && port == 443) ||
      (url.scheme() == QStringLiteral("http") && port == 80);
  if (!defaultPort)
    host += ':' + QByteArray::number(port);
  return host;
}

// The query exactly as sent, split back into encoded pairs and sorted.
QByteArray canonicalQuery(const QUrl &url) {
  const QByteArray query = url.query(QUrl::FullyEncoded).toLatin1();
  if (query.isEmpty())
    return {};
  QList<QPair<QByteArray, QByteArray>> pairs;
  for (const QByteArray &item : query.split('&')) {
    const qsizetype equals = item.indexOf('=');
    const QByteArray key = equals < 0 ? item : item.left(equals);
    const QByteArray value = equals < 0 ? QByteArray() : item.mid(equals + 1);
    // Re-encode so "+" or lowercase escapes from elsewhere still match.
    pairs.append({uriEncode(QUrl::fromPercentEncoding(key), true),
                  uriEncode(QUrl::fromPercentEncoding(value), true)});
  }
  std::sort(pairs.begin(), pairs.end());
  QByteArray out;
  for (const auto &[key, value] : pairs) {
    if (!out.isEmpty())
      out += '&';
    out += key + '=' + value;
  }
  return out;
}

} // namespace

QByteArray payloadHash(const QByteArray &body) { return sha256Hex(body); }

QByteArray uriEncode(const QString &text, bool encodeSlash) {
  static const char hex[] = "0123456789ABCDEF";
  QByteArray out;
  for (const char c : text.toUtf8()) {
    const auto u = static_cast<unsigned char>(c);
    const bool unreserved = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
                            (u >= '0' && u <= '9') || u == '-' || u == '_' ||
                            u == '.' || u == '~';
    if (unreserved || (u == '/' && !encodeSlash)) {
      out += c;
    } else {
      out += '%';
      out += hex[u >> 4];
      out += hex[u & 0xF];
    }
  }
  return out;
}

QUrl buildUrl(const QString &scheme, const QString &host, int port,
              const QString &path,
              const QList<QPair<QString, QString>> &query) {
  QByteArray encoded = scheme.toLatin1() + "://" + QUrl::toAce(host);
  if (port > 0)
    encoded += ':' + QByteArray::number(port);
  encoded += uriEncode(
      path.startsWith(QLatin1Char('/')) ? path : QLatin1Char('/') + path,
      false);
  if (!query.isEmpty()) {
    QByteArray q;
    for (const auto &[key, value] : query) {
      if (!q.isEmpty())
        q += '&';
      q += uriEncode(key, true) + '=' + uriEncode(value, true);
    }
    encoded += '?' + q;
  }
  return QUrl::fromEncoded(encoded, QUrl::StrictMode);
}

QByteArray canonicalRequest(const QByteArray &method, const QUrl &url,
                            const Headers &headers,
                            const QByteArray &payloadHash,
                            QByteArray *signedHeaders) {
  QList<QPair<QByteArray, QByteArray>> canonicalHeaders;
  for (const auto &[name, value] : headers)
    canonicalHeaders.append({name.toLower(), value.simplified()});
  std::sort(canonicalHeaders.begin(), canonicalHeaders.end());

  QByteArray headerBlock;
  QByteArray names;
  for (const auto &[name, value] : canonicalHeaders) {
    headerBlock += name + ':' + value + '\n';
    if (!names.isEmpty())
      names += ';';
    names += name;
  }
  if (signedHeaders)
    *signedHeaders = names;

  QByteArray path = url.path(QUrl::FullyEncoded).toLatin1();
  if (path.isEmpty())
    path = "/";
  return method + '\n' + path + '\n' + canonicalQuery(url) + '\n' +
         headerBlock + '\n' + names + '\n' + payloadHash;
}

QByteArray signature(const QByteArray &canonical,
                     const Credentials &credentials, const QDateTime &now) {
  const QByteArray toSign = kAlgorithm + '\n' + amzDate(now) + '\n' +
                            scope(credentials, now) + '\n' +
                            sha256Hex(canonical);
  QByteArray key =
      hmac("AWS4" + credentials.secretKey.toUtf8(), dateStamp(now));
  key = hmac(key, credentials.region.toUtf8());
  key = hmac(key, credentials.service.toUtf8());
  key = hmac(key, "aws4_request");
  return hmac(key, toSign).toHex();
}

Headers sign(const QByteArray &method, const QUrl &url, Headers headers,
             const QByteArray &payloadHash, const Credentials &credentials,
             const QDateTime &now) {
  headers.append({"host", hostHeader(url)});
  headers.append({"x-amz-date", amzDate(now)});
  headers.append({"x-amz-content-sha256", payloadHash});

  QByteArray signedHeaders;
  const QByteArray canonical =
      canonicalRequest(method, url, headers, payloadHash, &signedHeaders);
  headers.append({"Authorization",
                  kAlgorithm + " Credential=" + credentials.accessKey.toUtf8() +
                      '/' + scope(credentials, now) +
                      ", SignedHeaders=" + signedHeaders +
                      ", Signature=" + signature(canonical, credentials, now)});
  return headers;
}

QUrl presign(const QByteArray &method, const QUrl &url,
             const Credentials &credentials, const QDateTime &now,
             int expiresSeconds) {
  QUrl signedUrl = url;
  QByteArray query = url.query(QUrl::FullyEncoded).toLatin1();
  const auto add = [&query](const QByteArray &key, const QString &value) {
    if (!query.isEmpty())
      query += '&';
    query += key + '=' + uriEncode(value, true);
  };
  add("X-Amz-Algorithm", QString::fromLatin1(kAlgorithm));
  add("X-Amz-Credential", credentials.accessKey + QLatin1Char('/') +
                              QString::fromLatin1(scope(credentials, now)));
  add("X-Amz-Date", QString::fromLatin1(amzDate(now)));
  add("X-Amz-Expires", QString::number(expiresSeconds));
  add("X-Amz-SignedHeaders", QStringLiteral("host"));
  signedUrl.setQuery(QString::fromLatin1(query), QUrl::StrictMode);

  const QByteArray canonical =
      canonicalRequest(method, signedUrl, {{"host", hostHeader(url)}},
                       unsignedPayload(), nullptr);
  add("X-Amz-Signature",
      QString::fromLatin1(signature(canonical, credentials, now)));
  signedUrl.setQuery(QString::fromLatin1(query), QUrl::StrictMode);
  return signedUrl;
}

} // namespace upload::sigv4
