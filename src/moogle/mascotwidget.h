/*
 * SPDX-FileCopyrightText: 2026 moogle contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MOOGLE_MASCOTWIDGET_H
#define MOOGLE_MASCOTWIDGET_H

#include <QPixmap>
#include <QWidget>

namespace ghostwriter
{
/**
 * Small translucent moogle that sits in the bottom-right corner of its
 * parent. It ignores the mouse so it never gets in the way of the text
 * underneath, and swaps artwork with the light/dark theme.
 */
class MascotWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MascotWidget(QWidget *parent);

    void setDarkMode(bool dark);

    /**
     * Whether the user wants the mascot. Kept separate from visibility so
     * distraction-free mode can hide it temporarily without losing the
     * preference.
     */
    void setEnabledByUser(bool enabled);
    void setSuppressed(bool suppressed);

    /**
     * Space to keep clear on the right edge, e.g. for a scroll bar.
     */
    void setRightInset(int px);

    /**
     * Re-anchors to the bottom-right corner of the parent and raises
     * above siblings.
     */
    void reposition();

protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QPixmap m_light;
    QPixmap m_dark;
    QPixmap m_scaled;
    qreal m_scaledDpr = 0.0;
    bool m_darkMode = false;
    bool m_enabledByUser = true;
    bool m_suppressed = false;
    int m_rightInset = 18;

    const QPixmap &source() const;
    void rebuildScaled();
    void updateVisibility();
};
} // namespace ghostwriter

#endif // MOOGLE_MASCOTWIDGET_H
