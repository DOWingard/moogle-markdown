/*
 * SPDX-FileCopyrightText: 2026 moogle contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QEvent>
#include <QPainter>

#include "mascotwidget.h"
#include "structuredlog.h"

namespace ghostwriter
{
namespace
{
constexpr int kMascotHeight = 64;
constexpr int kBottomInset = 12;
// Light artwork is pale on a pale page and the dark artwork is a black coat on
// a dark page, so the dark variant needs more opacity to stay equally subtle.
constexpr qreal kLightOpacity = 0.55;
constexpr qreal kDarkOpacity = 0.70;
const QString kContext = QStringLiteral("mascot");
} // namespace

MascotWidget::MascotWidget(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    setObjectName(QStringLiteral("moogleMascot"));

    if (!m_light.load(QStringLiteral(":/resources/mascot/moogle-light.png"))) {
        logEvent(LogLevel::Error, kContext, QStringLiteral("light mascot resource failed to load"), {{"resource", ":/resources/mascot/moogle-light.png"}});
    }

    if (!m_dark.load(QStringLiteral(":/resources/mascot/moogle-dark.png"))) {
        logEvent(LogLevel::Error, kContext, QStringLiteral("dark mascot resource failed to load"), {{"resource", ":/resources/mascot/moogle-dark.png"}});
    }

    if (parent) {
        parent->installEventFilter(this);
    }

    rebuildScaled();
    updateVisibility();
    logEvent(LogLevel::Debug, kContext, QStringLiteral("mascot created"), {{"lightLoaded", !m_light.isNull()}, {"darkLoaded", !m_dark.isNull()}});
}

void MascotWidget::setDarkMode(bool dark)
{
    if (dark == m_darkMode && !m_scaled.isNull()) {
        return;
    }

    m_darkMode = dark;
    rebuildScaled();
    reposition();
    update();
    logEvent(LogLevel::Debug, kContext, QStringLiteral("mascot theme changed"), {{"dark", dark}});
}

void MascotWidget::setEnabledByUser(bool enabled)
{
    m_enabledByUser = enabled;
    updateVisibility();
}

void MascotWidget::setSuppressed(bool suppressed)
{
    m_suppressed = suppressed;
    updateVisibility();
}

void MascotWidget::setRightInset(int px)
{
    m_rightInset = px;
    reposition();
}

void MascotWidget::reposition()
{
    QWidget *p = parentWidget();

    if (!p || m_scaled.isNull()) {
        return;
    }

    const QSize logical = (m_scaled.deviceIndependentSize()).toSize();
    resize(logical);
    move(p->width() - logical.width() - m_rightInset, p->height() - logical.height() - kBottomInset);
    raise();
}

void MascotWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    if (m_scaled.isNull()) {
        return;
    }

    if (!qFuzzyCompare(m_scaledDpr, devicePixelRatioF())) {
        rebuildScaled();
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setOpacity(m_darkMode ? kDarkOpacity : kLightOpacity);
    painter.drawPixmap(0, 0, m_scaled);
}

bool MascotWidget::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == parentWidget()) && ((QEvent::Resize == event->type()) || (QEvent::Show == event->type()))) {
        reposition();
    }

    return QWidget::eventFilter(watched, event);
}

const QPixmap &MascotWidget::source() const
{
    return (m_darkMode && !m_dark.isNull()) ? m_dark : m_light;
}

void MascotWidget::rebuildScaled()
{
    const QPixmap &src = source();

    if (src.isNull()) {
        m_scaled = QPixmap();
        logEvent(LogLevel::Warn, kContext, QStringLiteral("no mascot artwork available; mascot hidden"), {{"dark", m_darkMode}});
        return;
    }

    // Pre-scale once per theme/DPR change instead of on every paint.
    const qreal dpr = devicePixelRatioF();
    m_scaled = src.scaledToHeight(qRound(kMascotHeight * dpr), Qt::SmoothTransformation);
    m_scaled.setDevicePixelRatio(dpr);
    m_scaledDpr = dpr;
}

void MascotWidget::updateVisibility()
{
    const bool show = m_enabledByUser && !m_suppressed && !m_scaled.isNull();
    setVisible(show);

    if (show) {
        reposition();
    }
}
} // namespace ghostwriter
