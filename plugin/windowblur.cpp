#include "windowblur.h"

#include <KWindowEffects>
#include <QMetaProperty>
#include <QQuickWindow>
#include <QTimer>
#include <algorithm>
#include <cmath>

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
    emit blurBoundsChanged();
}

QRect WindowBlur::blurBounds() const
{
    return m_tracker ? m_tracker->region().boundingRect() : QRect();
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
    // WHY re-apply when shown: the keyboard window is hidden and shown again constantly; a shown window can get a
    // fresh surface from the compositor, and the request must be on it.
    connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
        if (visible) {
            m_region = QRegion();   // force the next update to send the request again
            scheduleUpdate();
        }
    });
    update();
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
    if (region == m_region)
        return;
    m_region = region;
    // WHY blur only (no enableBackgroundContrast): the owner's Plasma theme (LiquidGlass) turns the contrast
    // effect off and blur on ([ContrastEffect] enabled=false, [BlurBehindEffect] enabled=true), and Plasma's
    // panels follow the theme, so blur alone is what makes the keyboard look like his panels.
    // An empty region switches the blur off (panel hidden); a null region would mean "whole window", never sent.
    KWindowEffects::enableBlurBehind(m_window, !region.isEmpty(), region);
    emit regionChanged();
}
