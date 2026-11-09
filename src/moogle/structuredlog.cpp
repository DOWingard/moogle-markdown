/*
 * SPDX-FileCopyrightText: 2026 moogle contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QDateTime>
#include <QJsonDocument>

#include "structuredlog.h"

namespace ghostwriter
{
Q_LOGGING_CATEGORY(lcMoogle, "moogle")

void logEvent(LogLevel level, const QString &context, const QString &message, const QJsonObject &fields)
{
    QJsonObject record = fields;
    const char *levelName = "info";

    switch (level) {
    case LogLevel::Debug:
        levelName = "debug";
        break;
    case LogLevel::Info:
        levelName = "info";
        break;
    case LogLevel::Warn:
        levelName = "warn";
        break;
    case LogLevel::Error:
        levelName = "error";
        break;
    }

    record.insert("timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    record.insert("level", QString::fromLatin1(levelName));
    record.insert("context", context);
    record.insert("message", message);

    const QString json = QString::fromUtf8(QJsonDocument(record).toJson(QJsonDocument::Compact));

    switch (level) {
    case LogLevel::Debug:
        qCDebug(lcMoogle).noquote() << json;
        break;
    case LogLevel::Info:
        qCInfo(lcMoogle).noquote() << json;
        break;
    case LogLevel::Warn:
        qCWarning(lcMoogle).noquote() << json;
        break;
    case LogLevel::Error:
        qCCritical(lcMoogle).noquote() << json;
        break;
    }
}

ScopedTiming::ScopedTiming(const QString &context, const QString &operation, qint64 slowMs)
    : m_context(context)
    , m_operation(operation)
    , m_slowMs(slowMs)
{
    m_timer.start();
}

ScopedTiming::~ScopedTiming()
{
    const qint64 elapsed = m_timer.elapsed();
    m_fields.insert("operation", m_operation);
    m_fields.insert("durationMs", elapsed);
    logEvent((elapsed > m_slowMs) ? LogLevel::Info : LogLevel::Debug, m_context, QStringLiteral("operation timed"), m_fields);
}

void ScopedTiming::addField(const QString &key, const QJsonValue &value)
{
    m_fields.insert(key, value);
}
} // namespace ghostwriter
