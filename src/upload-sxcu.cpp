/** @fileoverview Reads .sxcu documents and expands their {function:arg}
 * templates. */
#include "upload-sxcu.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStringList>

namespace sxcu {

namespace {

Pairs pairsFrom(const QJsonValue &value) {
  Pairs pairs;
  const QJsonObject object = value.toObject();
  for (auto it = object.begin(); it != object.end(); ++it) {
    const QJsonValue v = it.value();
    pairs.append(
        {it.key(), v.isString() ? v.toString() : v.toVariant().toString()});
  }
  return pairs;
}

bool fail(QString *error, const QString &message) {
  if (error)
    *error = message;
  return false;
}

// Walks the .sxcu syntax one character at a time. Text reads up to a '|' or
// '}' when it's an argument (so the caller can see which one ended it), or to
// the end of the input at the top level, where those two are ordinary text.
class Expander {
public:
  Expander(const QString &text, const Context &context)
      : text_(text), context_(context) {}

  QString run() { return text(false); }

private:
  QString text(bool inArgument) {
    QString out;
    while (pos_ < text_.size()) {
      const QChar c = text_.at(pos_);
      if (c == QLatin1Char('\\') && pos_ + 1 < text_.size()) {
        out += text_.at(pos_ + 1);
        pos_ += 2;
      } else if (c == QLatin1Char('{')) {
        ++pos_;
        out += call();
      } else if (inArgument &&
                 (c == QLatin1Char('|') || c == QLatin1Char('}'))) {
        break;
      } else {
        out += c;
        ++pos_;
      }
    }
    return out;
  }

  // Just past a '{': the function name, then its arguments, through '}'.
  QString call() {
    QString name;
    while (pos_ < text_.size()) {
      const QChar c = text_.at(pos_);
      if (c == QLatin1Char('}') || c == QLatin1Char(':'))
        break;
      name += c;
      ++pos_;
    }

    QStringList args;
    if (pos_ < text_.size() && text_.at(pos_) == QLatin1Char(':')) {
      do {
        ++pos_; // past ':' or '|'
        args << text(true);
      } while (pos_ < text_.size() && text_.at(pos_) == QLatin1Char('|'));
    }
    if (pos_ < text_.size() && text_.at(pos_) == QLatin1Char('}'))
      ++pos_;
    return evaluate(name.trimmed().toLower(), args);
  }

  QString responseText() const {
    return context_.response ? QString::fromUtf8(context_.response->body)
                             : QString();
  }

  QString evaluate(const QString &name, const QStringList &args) const {
    if (name == QStringLiteral("response"))
      return responseText();
    if (name == QStringLiteral("responseurl"))
      return context_.response ? context_.response->url.toString() : QString();
    if (name == QStringLiteral("filename"))
      return context_.fileName;
    if (name == QStringLiteral("header")) {
      if (args.isEmpty() || !context_.response)
        return {};
      return QString::fromUtf8(context_.response->headers.value(
          args.first().trimmed().toLower().toUtf8()));
    }
    if (name == QStringLiteral("json")) {
      // {json:path} reads the response; {json:input|path} reads input.
      if (args.size() >= 2)
        return jsonPath(args.at(0).toUtf8(), args.at(1));
      return args.isEmpty() ? QString()
                            : jsonPath(responseText().toUtf8(), args.first());
    }
    if (name == QStringLiteral("regex"))
      return regex(args);
    if (name == QStringLiteral("random")) {
      if (args.isEmpty())
        return {};
      return args.at(QRandomGenerator::global()->bounded(int(args.size())));
    }
    if (name == QStringLiteral("base64"))
      return args.isEmpty()
                 ? QString()
                 : QString::fromLatin1(args.first().toUtf8().toBase64());
    return {};
  }

  // {regex:pattern}, {regex:pattern|group} or {regex:input|pattern|group},
  // where group is a number or a name and defaults to the whole match.
  QString regex(const QStringList &args) const {
    QString input;
    QString pattern;
    QString group;
    if (args.size() >= 3) {
      input = args.at(0);
      pattern = args.at(1);
      group = args.at(2);
    } else if (!args.isEmpty()) {
      input = responseText();
      pattern = args.at(0);
      if (args.size() == 2)
        group = args.at(1);
    }
    if (input.isEmpty() || pattern.isEmpty())
      return {};

    const QRegularExpressionMatch match =
        QRegularExpression(pattern).match(input);
    if (!match.hasMatch())
      return {};
    if (group.isEmpty())
      return match.captured(0);
    bool isNumber = false;
    const int index = group.toInt(&isNumber);
    return isNumber ? match.captured(index) : match.captured(group);
  }

  const QString &text_;
  const Context &context_;
  qsizetype pos_ = 0;
};

} // namespace

bool fromJson(const QByteArray &json, Destination *out, QString *error) {
  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
  if (!doc.isObject())
    return fail(error, parseError.error != QJsonParseError::NoError
                           ? parseError.errorString()
                           : QStringLiteral("not a custom uploader"));

  const QJsonObject o = doc.object();
  Destination d;
  d.name = o.value(QStringLiteral("Name")).toString().trimmed();
  d.requestMethod = o.value(QStringLiteral("RequestMethod"))
                        .toString(d.requestMethod)
                        .toUpper();
  d.requestUrl = o.value(QStringLiteral("RequestURL")).toString().trimmed();
  d.fileFormName = o.value(QStringLiteral("FileFormName")).toString();
  // Older configs name the file field without spelling out the body.
  d.body = o.value(QStringLiteral("Body"))
               .toString(d.fileFormName.isEmpty()
                             ? QStringLiteral("None")
                             : QStringLiteral("MultipartFormData"));
  d.parameters = pairsFrom(o.value(QStringLiteral("Parameters")));
  d.headers = pairsFrom(o.value(QStringLiteral("Headers")));
  d.arguments = pairsFrom(o.value(QStringLiteral("Arguments")));
  d.url = o.value(QStringLiteral("URL")).toString();
  d.thumbnailUrl = o.value(QStringLiteral("ThumbnailURL")).toString();
  d.deletionUrl = o.value(QStringLiteral("DeletionURL")).toString();
  d.errorMessage = o.value(QStringLiteral("ErrorMessage")).toString();

  if (d.requestUrl.isEmpty())
    return fail(error, QStringLiteral("it has no RequestURL"));
  if (d.body == QStringLiteral("MultipartFormData")) {
    if (d.fileFormName.isEmpty())
      return fail(error, QStringLiteral("it has no FileFormName"));
  } else if (d.body != QStringLiteral("Binary")) {
    return fail(error,
                QStringLiteral("its %1 body can't carry a file").arg(d.body));
  }
  if (d.name.isEmpty())
    d.name = QUrl(d.requestUrl).host();

  if (out)
    *out = d;
  return true;
}

QString expand(const QString &text, const Context &context) {
  return Expander(text, context).run();
}

QString jsonPath(const QByteArray &json, const QString &path) {
  const QJsonDocument doc = QJsonDocument::fromJson(json);
  if (doc.isNull())
    return {};
  QJsonValue value =
      doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object());

  QString rest = path.trimmed();
  if (rest.startsWith(QStringLiteral("$")))
    rest.remove(0, 1);

  // Each step is ".key", "key" (first), or "[index]".
  static const QRegularExpression step(
      QStringLiteral(R"(^\.?([^.\[\]]+)|^\[(\d+)\])"));
  while (!rest.isEmpty()) {
    const QRegularExpressionMatch match = step.match(rest);
    if (!match.hasMatch())
      return {};
    if (match.capturedLength(2) > 0) {
      const QJsonArray array = value.toArray();
      const int index = match.captured(2).toInt();
      if (!value.isArray() || index >= array.size())
        return {};
      value = array.at(index);
    } else {
      if (!value.isObject())
        return {};
      const QJsonObject object = value.toObject();
      const QString key = match.captured(1);
      if (!object.contains(key))
        return {};
      value = object.value(key);
    }
    rest.remove(0, match.capturedLength(0));
  }

  switch (value.type()) {
  case QJsonValue::String:
    return value.toString();
  case QJsonValue::Object:
    return QString::fromUtf8(
        QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
  case QJsonValue::Array:
    return QString::fromUtf8(
        QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
  case QJsonValue::Double: {
    const double d = value.toDouble();
    return d == qint64(d) ? QString::number(qint64(d)) : QString::number(d);
  }
  case QJsonValue::Bool:
    return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
  default:
    return {};
  }
}

} // namespace sxcu
