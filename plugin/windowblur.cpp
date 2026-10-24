// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "windowblur.h"

#include <KWindowEffects>
#include <KWindowSystem>
#include <QEvent>
#include <QLoggingCategory>
#include <QMetaProperty>
#include <QPlatformSurfaceEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QTimer>
#include <algorithm>
#include <cmath>

// WHY a logging category with info ON by default: the blur cannot be tested without KWin, so the journal is the
// only way to see, on the device, which area was asked for and whether the window was ready. Turn it down with
// QT_LOGGING_RULES="swipekb.blur.info=false" once the blur is confirmed working.
Q_LOGGING_CATEGORY(lcBlur, "swipekb.blur", QtInfoMsg)

namespace swipekb {
QRegion roundedRegion(const QRect &rect, int topLeft, int topRight, int bottomRight, int bottomLeft)
{
    if (rect.isEmpty())
        return {};
    const int h = rect.height(), w = rect.width();
    // WHY clamp: a radius larger than half the side would make the corner strips overlap and cross.
    const int maxR = std::min(w, h) / 2;
    auto clampR = [maxR](int r) { return std::clamp(r, 0, maxR); };
    topLeft = clampR(topLeft); topRight = clampR(topRight);
    bottomRight = clampR(bottomRight); bottomLeft = clampR(bottomLeft);
    // How far in from the edge the curve is on pixel row `row` (counted from the corner's flat edge) of a
    // corner with radius r. Measured at the row's middle (+0.5) so the strips hug the drawn curve.
    auto inset = [](int r, int row) {
        const double dy = r - row - 0.5;
        return int(std::lround(r - std::sqrt(std::max(0.0, double(r) * r - dy * dy))));
    };
    QRegion region;
    const int top = std::max(topLeft, topRight), bottom = std::max(bottomLeft, bottomRight);
    for (int row = 0; row < top; ++row) {
        const int l = row < topLeft ? inset(topLeft, row) : 0;
        const int r = row < topRight ? inset(topRight, row) : 0;
        region += QRect(rect.x() + l, rect.y() + row, w - l - r, 1);
    }
    if (h - top - bottom > 0)
        region += QRect(rect.x(), rect.y() + top, w, h - top - bottom);
    for (int row = 0; row < bottom; ++row) {   // row 0 = the very bottom pixel row
        const int l = row < bottomLeft ? inset(bottomLeft, row) : 0;
        const int r = row < bottomRight ? inset(bottomRight, row) : 0;
        region += QRect(rect.x() + l, rect.y() + h - 1 - row, w - l - r, 1);
    }
    return region;
}
}

// ---- WindowBlur (the request created by the layout) ----

WindowBlur::WindowBlur(QObject *parent) : QObject(parent) {}

void WindowBlur::setTarget(QQuickItem *target)
{
    if (m_target == target)
        return;
    QObject::disconnect(m_windowConn);
    m_target = target;
    emit targetChanged();
    if (!m_target)
        return;
    // WHY wait for the window: an item can be created before it is placed in a window; the blur is a window request.
    m_windowConn = connect(m_target, &QQuickItem::windowChanged, this, &WindowBlur::attach);
    attach();
}

void WindowBlur::attach()
{
    if (!m_target || !m_target->window() || m_tracker)
        return;
    m_tracker = PanelBlurTracker::forWindow(m_target->window(), m_target);
    connect(m_tracker, &PanelBlurTracker::regionChanged, this, &WindowBlur::blurBoundsChanged);
    if (m_edgeHighlight > 0)
        m_tracker->setEdgeHighlight(m_edgeHighlight);
    emit blurBoundsChanged();
}

QRect WindowBlur::blurBounds() const
{
    return m_tracker ? m_tracker->region().boundingRect() : QRect();
}

QRect WindowBlur::requestBounds() const
{
    return m_tracker ? m_tracker->requestRegion().boundingRect() : QRect();
}

void WindowBlur::setEdgeHighlight(qreal opacity)
{
    opacity = std::clamp<qreal>(opacity, 0, 1);
    if (qFuzzyCompare(opacity, m_edgeHighlight))
        return;
    m_edgeHighlight = opacity;
    emit edgeHighlightChanged();
    if (m_tracker)
        m_tracker->setEdgeHighlight(m_edgeHighlight);
}

// ---- PanelBlurTracker (one per window, keeps the blur matched to the panel) ----

PanelBlurTracker *PanelBlurTracker::forWindow(QQuickWindow *window, QQuickItem *anyItemInPanel)
{
    static const auto name = QStringLiteral("swipekbPanelBlur");
    if (auto *existing = window->findChild<PanelBlurTracker *>(name, Qt::FindDirectChildrenOnly))
        return existing;
    // The panel = the target's ancestor that sits directly in the window's root item. In Plasma Keyboard that is
    // main.qml's Kirigami.ShadowedRectangle (the visible, rounded keyboard background). WHY not the whole window:
    // the window is as tall as the screen (only the panel takes touches), so blurring the whole window would blur
    // the entire screen behind the keyboard.
    QQuickItem *panel = anyItemInPanel;
    while (panel->parentItem() && panel->parentItem() != window->contentItem())
        panel = panel->parentItem();
    auto *t = new PanelBlurTracker(window, panel);
    t->setObjectName(name);
    return t;
}

PanelBlurTracker::PanelBlurTracker(QQuickWindow *window, QQuickItem *panel)
    : QObject(window), m_window(window), m_panel(panel)
{
    // WHY these signals: the panel moves or resizes when the guess bar appears, the screen rotates, or the
    // full-width setting changes; the blur must follow or it would show beside the keyboard.
    for (auto sig : {&QQuickItem::xChanged, &QQuickItem::yChanged, &QQuickItem::widthChanged,
                     &QQuickItem::heightChanged, &QQuickItem::visibleChanged})
        connect(panel, sig, this, &PanelBlurTracker::scheduleUpdate);
    // The corner radii come from Kirigami's ShadowedRectangle (`radius`, and the `corners` group that overrides
    // it per corner). Read by name so this also works (as a plain rectangle) if the panel is some other item.
    const QMetaObject *mo = panel->metaObject();
    const int radiusIdx = mo->indexOfProperty("radius");
    if (radiusIdx >= 0 && mo->property(radiusIdx).hasNotifySignal())
        connect(panel, mo->property(radiusIdx).notifySignal(), this,
                metaObject()->method(metaObject()->indexOfSlot("scheduleUpdate()")));
    if (auto *corners = panel->property("corners").value<QObject *>()) {
        const int changedIdx = corners->metaObject()->indexOfSignal("changed()");
        if (changedIdx >= 0)
            connect(corners, corners->metaObject()->method(changedIdx), this,
                    metaObject()->method(metaObject()->indexOfSlot("scheduleUpdate()")));
    }
    // WHY re-apply when shown: the keyboard window is hidden and shown again constantly, and each show gets a fresh
    // surface. KWindowSystem itself re-sends the stored blur when the window is exposed again; re-computing here
    // as well catches a panel that changed size while hidden, and logs every show so the journal tells the story.
    connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
        qCInfo(lcBlur) << "keyboard window" << (visible ? "shown" : "hidden");
        if (visible) {
            m_request = QRegion();   // force the next update to send (and log) the request again
            scheduleUpdate();
        }
    });
    // WHY the mask: Plasma Keyboard's window is screen-tall, and it marks only the panel as touchable (its
    // "interactiveRegion", which becomes the window mask). KWin treats that touchable part as the window itself,
    // and our blur area must be measured from its corner (see update()). Re-sent whenever it changes.
    const int regionSig = window->metaObject()->indexOfSignal("interactiveRegionChanged()");
    if (regionSig >= 0)
        connect(window, window->metaObject()->method(regionSig), this,
                metaObject()->method(metaObject()->indexOfSlot("scheduleUpdate()")));
    window->installEventFilter(this);
    qCInfo(lcBlur) << "blur tracker on" << window << "panel" << panel << "wayland:" << KWindowSystem::isPlatformWayland()
                   << "blur available:" << KWindowEffects::isEffectAvailable(KWindowEffects::BlurBehind);
    update();
}

bool PanelBlurTracker::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window) {
        if (event->type() == QEvent::Expose && m_window->isExposed()) {
            // Exposed = the window has a live surface on screen; KWindowSystem re-sends the blur at this moment.
            m_request = QRegion();
            scheduleUpdate();
        } else if (event->type() == QEvent::PlatformSurface) {
            const auto type = static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType();
            qCInfo(lcBlur) << "keyboard window surface"
                           << (type == QPlatformSurfaceEvent::SurfaceCreated ? "created" : "about to be destroyed");
        }
    }
    return QObject::eventFilter(watched, event);
}

void PanelBlurTracker::setEdgeHighlight(qreal opacity)
{
    if (!m_panel)
        return;
    // WHY a 1 px white line: Plasma's panel (his LiquidGlass theme) has a faint white rim (18% white) that makes the
    // glass edge catch the light; the keyboard gets the same along its top edge. A child of the panel, so it moves
    // and resizes with it; created once per window, like the blur itself.
    if (!m_edge && opacity > 0) {
        QQmlEngine *engine = qmlEngine(m_panel);
        if (!engine)
            return;
        QQmlComponent c(engine);
        c.setData("import QtQuick\nRectangle { objectName: \"swipekbEdgeHighlight\"; height: 1; z: 1000; color: \"white\" }",
                  QUrl());
        m_edge = qobject_cast<QQuickItem *>(c.create());
        if (!m_edge)
            return;
        m_edge->setParent(m_panel);
        m_edge->setParentItem(m_panel);
        // Follow the panel's width (top corners: the full-width keyboard has square ones, so a straight line fits).
        auto fit = [this] { if (m_edge && m_panel) m_edge->setWidth(m_panel->width()); };
        connect(m_panel, &QQuickItem::widthChanged, m_edge, fit);
        fit();
    }
    if (m_edge) {
        m_edge->setOpacity(opacity);
        m_edge->setVisible(opacity > 0);
    }
}

void PanelBlurTracker::scheduleUpdate()
{
    // WHY batch: one resize changes width and height (and maybe y) one after another; send KWin one request.
    if (m_pending)
        return;
    m_pending = true;
    QTimer::singleShot(0, this, &PanelBlurTracker::update);
}

void PanelBlurTracker::update()
{
    m_pending = false;
    if (!m_window || !m_panel)
        return;
    QRegion region;
    if (m_panel->isVisible() && m_panel->width() > 0 && m_panel->height() > 0) {
        const QRectF r = m_panel->mapRectToScene(QRectF(0, 0, m_panel->width(), m_panel->height()));
        const QRect rect(qRound(r.x()), qRound(r.y()), qRound(r.width()), qRound(r.height()));
        const qreal radius = std::max<qreal>(0, m_panel->property("radius").toReal());
        auto corner = [&](const char *name) {
            // Kirigami's corner values are -1 when not set, meaning "use radius".
            const QObject *corners = m_panel->property("corners").value<QObject *>();
            const QVariant v = corners ? corners->property(name) : QVariant();
            const qreal c = v.isValid() ? v.toReal() : -1;
            return int(std::lround(c >= 0 ? c : radius));
        };
        region = swipekb::roundedRegion(rect, corner("topLeftRadius"), corner("topRightRadius"),
                                        corner("bottomRightRadius"), corner("bottomLeftRadius"));
    }
    // WHY move the area: KWin (6.7, src/inputpanelv1window.cpp resetPosition) uses the touchable part of an input
    // panel (the window mask = the panel) as the window's frame, while the blur plugin (src/plugins/blur/blur.cpp,
    // blurRegion) reads our area as measured from that frame's corner and cuts away anything outside it. Measured
    // from the screen-tall window's top instead, the panel's area lies below the frame and is cut away entirely:
    // the keyboard was see-through but never blurred. So the area is shifted by the mask's top-left corner.
    // (If a future KWin starts measuring from the surface instead, the blur would vanish again: remove the shift.)
    const QRect mask = m_window->mask().boundingRect();
    const QPoint frameOrigin = mask.isEmpty() ? QPoint() : mask.topLeft();
    const QRegion request = region.translated(-frameOrigin);
    if (region == m_region && request == m_request)
        return;
    m_region = region;
    m_request = request;
    qCInfo(lcBlur).nospace() << "blur " << (request.isEmpty() ? "OFF" : "ON") << ": window " << m_window
                             << " platformWindow=" << (m_window->handle() != nullptr) << " exposed=" << m_window->isExposed()
                             << " panel(window coords)=" << region.boundingRect() << " mask=" << mask
                             << " sent(frame coords)=" << request.boundingRect() << " rects=" << request.rectCount()
                             << " wayland=" << KWindowSystem::isPlatformWayland()
                             << " blurAvailable=" << KWindowEffects::isEffectAvailable(KWindowEffects::BlurBehind);
    // WHY blur only (no enableBackgroundContrast): the owner's Plasma theme (LiquidGlass) turns the contrast
    // effect off and blur on ([ContrastEffect] enabled=false, [BlurBehindEffect] enabled=true), and Plasma's
    // panels follow the theme, so blur alone is what makes the keyboard look like his panels.
    // An empty region switches the blur off (panel hidden); a null region would mean "whole window", never sent.
    KWindowEffects::enableBlurBehind(m_window, !request.isEmpty(), request);
    emit regionChanged();
}
