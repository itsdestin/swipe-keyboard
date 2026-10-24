/*
 * SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

// WindowBlur — asks KWin to blur what is behind the keyboard panel (the frosted look of Plasma's panels).
// WHY it exists: touch-profile can make the keyboard translucent, but translucent keys over busy app text are
// hard to read. Plasma's panels solve that by asking KWin to blur the area behind them; this does the same for
// the keyboard. Nothing here paints anything: it only tells KWin which part of the window to blur.
//
// Usage (touch-profile's layout hook, only when its TP_KBD_GLASS=1):  WindowBlur { target: keyboard }
// `target` is ANY item inside the keyboard panel. The blur itself is held by a helper attached to the window
// (one per window), so it outlives this object: the layout that creates it is destroyed on "?123" and other
// layout switches, and the blur must stay on for every page.
#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QRegion>
#include <QtQml/qqmlregistration.h>

namespace swipekb {
// The blur area for a rectangle with rounded corners (radii in logical px: top-left, top-right, bottom-right,
// bottom-left). WHY a stack of rectangles: KWin's blur request only takes a region (a set of rectangles), with
// no rounded-corner option, so each corner row is its own one-pixel-high strip that stops where the curve does.
// This is how Plasma's own panels describe their rounded shape to KWin too (a mask made of pixels).
QRegion roundedRegion(const QRect &rect, int topLeft, int topRight, int bottomRight, int bottomLeft);
}

class PanelBlurTracker;

class WindowBlur : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickItem *target READ target WRITE setTarget NOTIFY targetChanged)
    // The area currently blurred, in window coordinates (bounding box). For tests and debugging only.
    Q_PROPERTY(QRect blurBounds READ blurBounds NOTIFY blurBoundsChanged)
    // The same area as actually sent to KWin (bounding box), i.e. moved to KWin's idea of where the window starts
    // (see PanelBlurTracker::update). For tests and debugging only.
    Q_PROPERTY(QRect requestBounds READ requestBounds NOTIFY blurBoundsChanged)
    // Opacity (0..1) of a thin white line along the panel's top edge, like the bright rim of Plasma's panels.
    // 0 (default) = no line. Set once per window: the first WindowBlur with a value > 0 decides.
    Q_PROPERTY(qreal edgeHighlight READ edgeHighlight WRITE setEdgeHighlight NOTIFY edgeHighlightChanged)

public:
    explicit WindowBlur(QObject *parent = nullptr);

    QQuickItem *target() const { return m_target; }
    void setTarget(QQuickItem *target);
    QRect blurBounds() const;
    QRect requestBounds() const;
    qreal edgeHighlight() const { return m_edgeHighlight; }
    void setEdgeHighlight(qreal opacity);

signals:
    void targetChanged();
    void blurBoundsChanged();
    void edgeHighlightChanged();

private:
    void attach();

    QPointer<QQuickItem> m_target;
    QPointer<PanelBlurTracker> m_tracker;
    QMetaObject::Connection m_windowConn;
    qreal m_edgeHighlight = 0;
};

// The per-window helper. Lives as a child of the window, so it lasts as long as the keyboard window does.
class PanelBlurTracker : public QObject {
    Q_OBJECT

public:
    // Returns the window's tracker, creating it the first time (so repeated layout loads never stack blurs).
    static PanelBlurTracker *forWindow(QQuickWindow *window, QQuickItem *anyItemInPanel);

    QRegion region() const { return m_region; }
    QRegion requestRegion() const { return m_request; }
    void setEdgeHighlight(qreal opacity);

signals:
    void regionChanged();

protected:
    // WHY watch the window's events: the keyboard window gets a new on-screen surface each time it is shown, and
    // only an exposed window can carry the blur. Logged and re-checked here so the journal shows every step.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    PanelBlurTracker(QQuickWindow *window, QQuickItem *panel);
    Q_SLOT void scheduleUpdate();   // a slot so it can be connected by name to the panel's radius signals
    void update();

    QPointer<QQuickWindow> m_window;
    QPointer<QQuickItem> m_panel;
    QPointer<QQuickItem> m_edge;    // the optional highlight line (a child of the panel)
    QRegion m_region;               // the panel's shape in window coordinates
    QRegion m_request;              // the same shape as sent to KWin (see update())
    bool m_pending = false;
};
