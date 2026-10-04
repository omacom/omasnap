/** @fileoverview ShareX custom uploader (.sxcu) documents and their template
 * syntax. */
#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>

// Upload destinations described in ShareX's custom uploader format (.sxcu),
// so the many configs people already share for their hosts work here too.
namespace sxcu {

using Pairs = QList<QPair<QString, QString>>;

// One destination. Only the file-carrying bodies are supported, since omasnap
// only ever uploads a screenshot: "MultipartFormData" (the file in
// FileFormName, alongside Arguments) and "Binary" (the file as the raw request
// body).
struct Destination {
  QString name;
  QString requestMethod = QStringLiteral("POST");
  QString requestUrl;
  QString body = QStringLiteral("MultipartFormData");
  QString fileFormName;
  Pairs parameters;
  Pairs headers;
  Pairs arguments;
  // Templates run against the response; an empty url means the whole
  // response body is the link.
  QString url;
  QString thumbnailUrl;
  QString deletionUrl;
  QString errorMessage;
};

// Reads a .sxcu document. Returns false with a reason when it isn't one or
// can't upload a file.
bool fromJson(const QByteArray &json, Destination *out,
              QString *error = nullptr);

// What the templates can look at once the host has answered.
struct Response {
  int status = 0;
  QByteArray body;
  QUrl url;
  // Keyed by lower-cased header name.
  QHash<QByteArray, QByteArray> headers;
};

struct Context {
  const Response *response = nullptr;
  QString fileName;
};

// Expands the .sxcu syntax: {function} or {function:arg|arg}, nestable, with
// backslash escaping the special characters. Supported functions are
// response, responseurl, header, json, regex, filename, random and base64;
// unknown ones expand to nothing.
QString expand(const QString &text, const Context &context);

// Looks a dotted/indexed path such as "files[0].url" (an optional leading
// "$." is ignored) up in a JSON document. Strings come back bare, other
// values as compact JSON; a missing path gives an empty string.
QString jsonPath(const QByteArray &json, const QString &path);

} // namespace sxcu
