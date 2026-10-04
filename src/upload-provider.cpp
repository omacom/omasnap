/** @fileoverview Provider registry and shared field handling. */
#include "upload-provider.hpp"

namespace upload {

namespace {

QString typeName(Field::Type type) {
  switch (type) {
  case Field::Text:
    return QStringLiteral("text");
  case Field::Multiline:
    return QStringLiteral("multiline");
  case Field::Secret:
    return QStringLiteral("secret");
  case Field::Number:
    return QStringLiteral("number");
  case Field::Toggle:
    return QStringLiteral("toggle");
  case Field::Choice:
    return QStringLiteral("choice");
  case Field::Hosts:
    return QStringLiteral("hosts");
  }
  return QStringLiteral("text");
}

bool isBlank(const QVariant &value) {
  if (value.typeId() == QMetaType::QVariantList ||
      value.typeId() == QMetaType::QStringList)
    return value.toList().isEmpty();
  return value.toString().trimmed().isEmpty();
}

} // namespace

QVariantMap Field::toVariant() const {
  return {
      {QStringLiteral("key"), key},
      {QStringLiteral("label"), label},
      {QStringLiteral("type"), typeName(type)},
      {QStringLiteral("defaultValue"), defaultValue},
      {QStringLiteral("choices"), choices},
      {QStringLiteral("placeholder"), placeholder},
      {QStringLiteral("help"), help},
      {QStringLiteral("required"), required},
      {QStringLiteral("hidden"), hidden},
  };
}

QString Provider::validate(const QVariantMap &settings) const {
  for (const Field &field : fields()) {
    if (field.required && isBlank(settings.value(field.key)))
      return QStringLiteral("%1 is required.").arg(field.label);
  }
  return {};
}

Authorization *Provider::authorize(const QVariantMap &, const Services &,
                                   QObject *) const {
  return nullptr;
}

QVariantMap Provider::withDefaults(const QVariantMap &settings) const {
  QVariantMap merged = settings;
  for (const Field &field : fields()) {
    if (!merged.contains(field.key) && field.defaultValue.isValid())
      merged.insert(field.key, field.defaultValue);
  }
  return merged;
}

const QList<const Provider *> &providers() {
  static const QList<const Provider *> all = {
      s3Provider(),     dropboxProvider(),   nextcloudProvider(),
      immichProvider(), xbackboneProvider(), ftpProvider(),
      imgurProvider(),  sxcuProvider(),
  };
  return all;
}

const Provider *findProvider(const QString &id) {
  for (const Provider *provider : providers()) {
    if (provider->id() == id)
      return provider;
  }
  return nullptr;
}

} // namespace upload
