/*
 * SPDX-FileCopyrightText: 2026 moogle contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MOOGLE_STRUCTUREDLOG_H
#define MOOGLE_STRUCTUREDLOG_H

#include <QElapsedTimer>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QString>

namespace ghostwriter
{
Q_DECLARE_LOGGING_CATEGORY(lcMoogle)

/**
 * Severity for structured log records. Maps onto Qt message types so records
 * flow through the application's existing message handler.
 */
enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error
};

/**
 * Emits one JSON log record:
 * {"timestamp", "level", "context", "message", ...fields}.
 * "error" and "stack" belong in fields when known.
 */
void logEvent(LogLevel level, const QString &context, const QString &message, const QJsonObject &fields = QJsonObject());

/**
 * Logs the duration of a scope at debug level (info when slower than
 * slowMs) when it is destroyed.
 */
class ScopedTiming
{
public:
    ScopedTiming(const QString &context, const QString &operation, qint64 slowMs = 100);
    ~ScopedTiming();

    void addField(const QString &key, const QJsonValue &value);

private:
    QString m_context;
    QString m_operation;
    qint64 m_slowMs;
    QJsonObject m_fields;
    QElapsedTimer m_timer;
};
} // namespace ghostwriter

#endif // MOOGLE_STRUCTUREDLOG_H
