/*
 * SPDX-FileCopyrightText: 2026 moogle contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RENDEREDVIEW_H
#define RENDEREDVIEW_H

#include <functional>

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QtWebEngineWidgets/QWebEngineView>

#include "editor/markdowndocument.h"

namespace ghostwriter
{
/**
 * Web channel object shared with renderedview.js. Calls from the page land
 * here and are re-emitted as signals for RenderedView.
 */
class RenderedViewBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString styleSheet READ styleSheet NOTIFY styleSheetChanged)

public:
    explicit RenderedViewBridge(QObject *parent = nullptr);

    QString styleSheet() const;
    void setStyleSheet(const QString &css);

    Q_INVOKABLE void pageReady();
    Q_INVOKABLE void presented(int token, const QString &resultJson);
    Q_INVOKABLE void requestToggle();
    Q_INVOKABLE void openLink(const QString &url);
    Q_INVOKABLE void log(const QString &level, const QString &message, const QString &fieldsJson);

signals:
    void styleSheetChanged(const QString &css);
    void ready();
    void presentFinished(int token, const QString &resultJson);
    void toggleRequested();

private:
    QString m_styleSheet;
};

/**
 * Read-only, caret-navigable rendering of the Markdown document that swaps
 * in place of the editor. Rendered with cmark-gfm source positions so the
 * page can translate scroll position, cursor and selection to and from
 * Markdown source offsets.
 *
 * Positions exchanged with the page are absolute QTextDocument character
 * positions (UTF-16 offsets into MarkdownDocument::toPlainText()).
 */
class RenderedView : public QWebEngineView
{
    Q_OBJECT

public:
    using PresentCallback = std::function<void(bool positioned)>;
    using CaptureCallback = std::function<void(const QJsonObject &state)>;

    RenderedView(MarkdownDocument *document, QWidget *parent = nullptr);
    virtual ~RenderedView();

    /**
     * Sets the theme CSS (same style sheet as the live preview).
     */
    void setPreviewStyleSheet(const QString &css);

    /**
     * Re-renders the document if its text changed since the last render,
     * then positions scroll, caret and selection from state:
     *   { scroll: {offset, fraction}, selection: {anchor, focus},
     *     contentWidth }
     * done(true) fires once the page has laid out and positioned itself;
     * done(false) on timeout or page failure (the view is still usable).
     */
    void present(const QJsonObject &state, PresentCallback done);

    /**
     * Reads the current state back from the page. Keys are omitted for parts
     * the user did not change since present(), so callers keep their exact
     * original values instead of a lossy round trip.
     */
    void capture(CaptureCallback done);

    void navigateToHeading(int headingSequenceNumber);

signals:
    /**
     * The user pressed the swap shortcut while the page had keyboard focus.
     */
    void toggleRequested();

private:
    struct PendingPresent {
        int token;
        QJsonObject payload;
        PresentCallback done;
    };

    MarkdownDocument *m_document;
    RenderedViewBridge *m_bridge;
    QString m_wrapperHtml;
    QString m_renderedSource;
    bool m_hasRendered = false;
    bool m_pageReady = false;
    int m_nextToken = 1;
    QList<PendingPresent> m_queued;
    QHash<int, PresentCallback> m_inFlight;

    void loadWrapperPage();
    void send(const PendingPresent &request);
    void onPresentFinished(int token, const QString &resultJson);
    void failAllInFlight(const QString &reason);
};
} // namespace ghostwriter

#endif // RENDEREDVIEW_H
