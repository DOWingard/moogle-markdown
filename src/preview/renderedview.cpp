/*
 * SPDX-FileCopyrightText: 2026 moogle contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <algorithm>
#include <memory>

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QPointer>
#include <QScreen>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineSettings>

#include "markdown/cmarkgfmapi.h"
#include "moogle/structuredlog.h"
#include "renderedview.h"
#include "sandboxedwebpage.h"

namespace ghostwriter
{
namespace
{
const QString kContext = QStringLiteral("renderedview");
// The first present() after creation also waits for Chromium to start and
// load the page, which can take a second or more on a cold start.
constexpr int kFirstPresentTimeoutMs = 5000;
constexpr int kPresentTimeoutMs = 1500;
constexpr int kCaptureTimeoutMs = 1000;

QJsonObject parseObject(const QString &json, const QString &what)
{
    if (json.isEmpty()) {
        return QJsonObject();
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);

    if (QJsonParseError::NoError != err.error || !doc.isObject()) {
        logEvent(LogLevel::Warn,
                 kContext,
                 QStringLiteral("unparseable JSON from page"),
                 {{"what", what}, {"error", err.errorString()}, {"length", json.length()}});
        return QJsonObject();
    }

    return doc.object();
}
} // namespace

RenderedViewBridge::RenderedViewBridge(QObject *parent)
    : QObject(parent)
{
}

QString RenderedViewBridge::styleSheet() const
{
    return m_styleSheet;
}

void RenderedViewBridge::setStyleSheet(const QString &css)
{
    m_styleSheet = css;
    emit styleSheetChanged(m_styleSheet);
}

void RenderedViewBridge::pageReady()
{
    emit ready();
}

void RenderedViewBridge::presented(int token, const QString &resultJson)
{
    emit presentFinished(token, resultJson);
}

void RenderedViewBridge::requestToggle()
{
    emit toggleRequested();
}

void RenderedViewBridge::openLink(const QString &url)
{
    const QUrl target(url);
    const QString scheme = target.scheme().toLower();

    // Only hand well-known, non-executable schemes to the desktop.
    if (!target.isValid() || !(scheme == "http" || scheme == "https" || scheme == "mailto" || scheme == "file")) {
        logEvent(LogLevel::Warn, kContext, QStringLiteral("refused to open link with unsupported scheme"), {{"scheme", scheme}});
        return;
    }

    if (!QDesktopServices::openUrl(target)) {
        logEvent(LogLevel::Warn, kContext, QStringLiteral("desktop failed to open link"), {{"scheme", scheme}});
    } else {
        logEvent(LogLevel::Info, kContext, QStringLiteral("opened link from rendered view"), {{"scheme", scheme}});
    }
}

void RenderedViewBridge::log(const QString &level, const QString &message, const QString &fieldsJson)
{
    LogLevel lvl = LogLevel::Debug;

    if (level == "info") {
        lvl = LogLevel::Info;
    } else if (level == "warn") {
        lvl = LogLevel::Warn;
    } else if (level == "error") {
        lvl = LogLevel::Error;
    }

    logEvent(lvl, QStringLiteral("renderedview.js"), message, parseObject(fieldsJson, QStringLiteral("log fields")));
}

RenderedView::RenderedView(MarkdownDocument *document, QWidget *parent)
    : QWebEngineView(parent)
    , m_document(document)
    , m_bridge(new RenderedViewBridge(this))
{
    setObjectName(QStringLiteral("renderedview"));
    setPage(new SandboxedWebPage(this));
    settings()->setDefaultTextEncoding("utf-8");
    settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    page()->action(QWebEnginePage::Reload)->setVisible(false);
    page()->action(QWebEnginePage::ReloadAndBypassCache)->setVisible(false);
    page()->action(QWebEnginePage::OpenLinkInThisWindow)->setVisible(false);
    page()->action(QWebEnginePage::OpenLinkInNewWindow)->setVisible(false);
    page()->action(QWebEnginePage::ViewSource)->setVisible(false);
    page()->action(QWebEnginePage::SavePage)->setVisible(false);

    // Chromium assumes 96 DPI; match the live preview's scaling.
    setZoomFactor(QGuiApplication::primaryScreen()->logicalDotsPerInchX() / 96.0);

    QWebChannel *channel = new QWebChannel(this);
    channel->registerObject(QStringLiteral("moogleBridge"), m_bridge);
    page()->setWebChannel(channel);

    QFile wrapper(QStringLiteral(":/resources/renderedview.html"));

    if (!wrapper.open(QFile::ReadOnly | QFile::Text)) {
        logEvent(LogLevel::Error, kContext, QStringLiteral("could not open rendered view page resource"), {{"error", wrapper.errorString()}});
        m_wrapperHtml = tr("Error loading resources/renderedview.html");
    } else {
        QTextStream stream(&wrapper);
        m_wrapperHtml = stream.readAll();
    }

    connect(m_bridge, &RenderedViewBridge::ready, this, [this]() {
        m_pageReady = true;
        logEvent(LogLevel::Info, kContext, QStringLiteral("rendered view page ready"), {{"queued", m_queued.size()}});

        const auto queued = m_queued;
        m_queued.clear();

        for (const auto &request : queued) {
            send(request);
        }
    });
    connect(m_bridge, &RenderedViewBridge::presentFinished, this, &RenderedView::onPresentFinished);
    connect(m_bridge, &RenderedViewBridge::toggleRequested, this, &RenderedView::toggleRequested);

    connect(this, &QWebEngineView::loadFinished, this, [](bool ok) {
        if (!ok) {
            logEvent(LogLevel::Error, kContext, QStringLiteral("rendered view page failed to load"));
        }
    });

    connect(page(), &QWebEnginePage::renderProcessTerminated, this, [this](QWebEnginePage::RenderProcessTerminationStatus status, int exitCode) {
        logEvent(LogLevel::Error,
                 kContext,
                 QStringLiteral("render process terminated; reloading rendered view"),
                 {{"status", static_cast<int>(status)}, {"exitCode", exitCode}});
        failAllInFlight(QStringLiteral("render process terminated"));
        loadWrapperPage();
    });

    connect(document, &MarkdownDocument::filePathChanged, this, [this]() {
        // Relative image paths resolve against the document's folder, which
        // is baked into the page's base URL.
        loadWrapperPage();
    });

    loadWrapperPage();
}

RenderedView::~RenderedView()
{
    m_inFlight.clear();
    m_queued.clear();
}

void RenderedView::setPreviewStyleSheet(const QString &css)
{
    m_bridge->setStyleSheet(css);
}

void RenderedView::present(const QJsonObject &state, PresentCallback done)
{
    QJsonObject payload = state;
    const QString text = m_document->toPlainText();

    if (!m_hasRendered || text != m_renderedSource) {
        ScopedTiming timing(kContext, QStringLiteral("render markdown"), 50);
        timing.addField("chars", static_cast<qint64>(text.length()));

        // Smart typography matches what the live preview shows.
        payload.insert("html", CmarkGfmAPI::instance()->renderToHtml(text, true, true));
        payload.insert("source", text);
        m_renderedSource = text;
        m_hasRendered = true;
    }

    payload.insert("expectedWidth", width() / zoomFactor());

    const int token = m_nextToken++;
    const int timeoutMs = m_pageReady ? kPresentTimeoutMs : kFirstPresentTimeoutMs;
    auto shared = std::make_shared<PresentCallback>(std::move(done));
    auto once = [shared](bool ok) {
        if (*shared) {
            auto cb = std::move(*shared);
            *shared = nullptr;
            cb(ok);
        }
    };

    PendingPresent request{token, payload, once};

    // A timeout releases the caller but leaves the request in place so the
    // content still arrives if the page is merely slow.
    QTimer::singleShot(timeoutMs, this, [this, token, once, timeoutMs]() {
        if (m_inFlight.contains(token) || std::any_of(m_queued.cbegin(), m_queued.cend(), [token](const PendingPresent &p) {
                return p.token == token;
            })) {
            logEvent(LogLevel::Warn,
                     kContext,
                     QStringLiteral("present timed out; showing view before positioning finished"),
                     {{"token", token}, {"timeoutMs", timeoutMs}, {"pageReady", m_pageReady}});
            once(false);
        }
    });

    if (!m_pageReady) {
        logEvent(LogLevel::Debug, kContext, QStringLiteral("present queued until page is ready"), {{"token", token}});
        m_queued.append(request);
        return;
    }

    send(request);
}

void RenderedView::capture(CaptureCallback done)
{
    if (!m_pageReady) {
        logEvent(LogLevel::Warn, kContext, QStringLiteral("capture requested before page ready; returning empty state"));
        done(QJsonObject());
        return;
    }

    auto shared = std::make_shared<CaptureCallback>(std::move(done));
    auto once = [shared](const QJsonObject &state) {
        if (*shared) {
            auto cb = std::move(*shared);
            *shared = nullptr;
            cb(state);
        }
    };

    QTimer::singleShot(kCaptureTimeoutMs, this, [once, shared]() {
        if (*shared) {
            logEvent(LogLevel::Warn, kContext, QStringLiteral("capture timed out; keeping editor state"), {{"timeoutMs", kCaptureTimeoutMs}});
            once(QJsonObject());
        }
    });

    page()->runJavaScript(QStringLiteral("window.moogle ? window.moogle.capture() : ''"), [once](const QVariant &result) {
        once(parseObject(result.toString(), QStringLiteral("capture")));
    });
}

void RenderedView::navigateToHeading(int headingSequenceNumber)
{
    page()->runJavaScript(QStringLiteral("window.moogle && window.moogle.scrollToHeading(%1);").arg(headingSequenceNumber));
}

void RenderedView::loadWrapperPage()
{
    m_pageReady = false;
    m_hasRendered = false;
    failAllInFlight(QStringLiteral("page reloading"));

    QString baseUrl;
    const QString filePath = m_document->filePath();

    if (!filePath.isEmpty()) {
        // Trailing slash is required for Chromium to treat it as a directory.
        baseUrl = QUrl::fromLocalFile(QFileInfo(filePath).dir().absolutePath() + "/").toString();
    }

    logEvent(LogLevel::Debug, kContext, QStringLiteral("loading rendered view page"), {{"hasBaseUrl", !baseUrl.isEmpty()}});
    setHtml(m_wrapperHtml, QUrl(baseUrl));
}

void RenderedView::send(const PendingPresent &request)
{
    m_inFlight.insert(request.token, request.done);

    QJsonObject payload = request.payload;
    payload.insert("token", request.token);

    // JSON is a valid JavaScript expression, so it can be passed inline.
    const QString script =
        QStringLiteral("window.moogle && window.moogle.present(%1);").arg(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));

    logEvent(LogLevel::Debug,
             kContext,
             QStringLiteral("sending present to page"),
             {{"token", request.token}, {"withHtml", payload.contains("html")}, {"scriptChars", static_cast<qint64>(script.length())}});
    page()->runJavaScript(script);
}

void RenderedView::onPresentFinished(int token, const QString &resultJson)
{
    const QJsonObject result = parseObject(resultJson, QStringLiteral("present result"));
    const bool ok = result.value("ok").toBool(false);

    QJsonObject fields = result;
    fields.insert("token", token);
    logEvent(ok ? LogLevel::Debug : LogLevel::Warn,
             kContext,
             ok ? QStringLiteral("page positioned") : QStringLiteral("page reported positioning failure"),
             fields);

    PresentCallback cb = m_inFlight.take(token);

    if (cb) {
        cb(ok);
    }
}

void RenderedView::failAllInFlight(const QString &reason)
{
    if (m_inFlight.isEmpty()) {
        return;
    }

    logEvent(LogLevel::Warn, kContext, QStringLiteral("abandoning in-flight presents"), {{"reason", reason}, {"count", m_inFlight.size()}});

    const auto callbacks = m_inFlight.values();
    m_inFlight.clear();

    for (const auto &cb : callbacks) {
        if (cb) {
            cb(false);
        }
    }
}
} // namespace ghostwriter
