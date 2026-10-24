// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "swipehook.h"

#include "editorport.h"
#include "swipecontroller.h"

#include <QLoggingCategory>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>

Q_LOGGING_CATEGORY(lcHook69, "swipekb.plasma69.hook", QtInfoMsg)

namespace
{
// WHY properties, not class names: KDE's Keyboard is a QML file (Keyboard.qml) whose type name is not stable API;
// what we need from it is exactly these properties.
const char *const kKeyboardProperties[] = {"inputEngine", "keyboardController", "layoutType", "packageId"};

bool isKeyboard(const QObject *o)
{
    if (!o || !qobject_cast<const QQuickItem *>(o)) {
        return false;
    }
    for (const char *p : kKeyboardProperties) {
        if (o->metaObject()->indexOfProperty(p) < 0) {
            return false;
        }
    }
    return true;
}

QObject *findUp(QQuickItem *from, const char *property)
{
    for (QQuickItem *item = from; item; item = item->parentItem()) {
        if (auto *o = item->property(property).value<QObject *>()) {
            return o;
        }
    }
    if (from && from->window()) {
        return from->window()->property(property).value<QObject *>();
    }
    return nullptr;
}

QStringList ourPackagesFromEnvironment()
{
    // Set by the launcher to the package ids it generated (integration/plasma69.py).
    return qEnvironmentVariable("SWIPEKB_PLASMA69_PACKAGES").split(QLatin1Char(','), Qt::SkipEmptyParts);
}

} // namespace

SwipeHook::SwipeHook(QObject *parent)
    : QObject(parent)
{
}

QQuickItem *SwipeHook::keyboardRoot(QObject *anchor)
{
    // A main layout (KeyboardLayout) is an item inside the Keyboard; MR !179's KeyboardLayoutDefinition is a plain
    // QtObject whose QObject parent is the Loader that made it. Walk both chains.
    for (QObject *o = anchor; o; o = o->parent()) {
        if (auto *item = qobject_cast<QQuickItem *>(o)) {
            for (QQuickItem *i = item; i; i = i->parentItem()) {
                if (isKeyboard(i)) {
                    return i;
                }
            }
        }
    }
    return nullptr;
}

QQuickItem *SwipeHook::layoutLoader(QQuickItem *keyboard)
{
    if (!keyboard) {
        return nullptr;
    }
    // WHY "a Loader one level down inside the Keyboard's column": KDE's Keyboard.qml holds the layout Loader inside its
    // ColumnLayout (under the candidate strip); MR !179 adds a second Loader for the definition directly under the root.
    for (QQuickItem *child : keyboard->childItems()) {
        for (QQuickItem *grandChild : child->childItems()) {
            if (grandChild->inherits("QQuickLoader")) {
                return grandChild;
            }
        }
    }
    return nullptr;
}

QStringList SwipeHook::missingInternals(QObject *anchor) const
{
    QStringList missing;
    QQuickItem *keyboard = keyboardRoot(anchor);
    if (!keyboard) {
        return {QStringLiteral("Keyboard item with inputEngine/keyboardController/layoutType/packageId")};
    }
    if (!layoutLoader(keyboard)) {
        missing << QStringLiteral("layout Loader inside the Keyboard's column");
    }
    missing << InputEngineEditor::missingMembers(keyboard->property("inputEngine").value<QObject *>());
    if (!qmlEngine(keyboard)) {
        missing << QStringLiteral("QML engine of the Keyboard");
    }
    return missing;
}

void SwipeHook::watch(QObject *layout)
{
    QPointer<QObject> guard(layout);
    // WHY one turn later: the wrapper's Component.onCompleted runs while KDE's Loader is still creating it (with MR
    // !179 even for a layout that is about to be replaced), before its parent and context are set.
    QTimer::singleShot(0, this, [this, guard] {
        if (guard) {
            attach(guard);
        }
    });
}

void SwipeHook::attach(QObject *layout)
{
    QQuickItem *keyboard = keyboardRoot(layout);
    const QStringList missing = missingInternals(layout);
    if (!keyboard || !missing.isEmpty()) {
        // WHY loud but harmless: a KDE change shows up in the log and our tests, while typing keeps working stock.
        qCWarning(lcHook69) << "Swipe Keyboard stays inactive; Plasma Keyboard internals changed:" << missing;
        return;
    }
    const QString packageId = keyboard->property("packageId").toString();
    if (auto existing = m_attached.value(keyboard)) {
        existing->addPackage(packageId);
        existing->decorate();
        return;
    }
    QStringList packages = ourPackagesFromEnvironment();
    if (!packageId.isEmpty() && !packages.contains(packageId)) {
        packages << packageId;
    }
    auto *attachment = new KeyboardAttachment(keyboard, layoutLoader(keyboard), packages, keyboard);
    m_attached.insert(keyboard, attachment);
    connect(keyboard, &QObject::destroyed, this, [this, keyboard] {
        m_attached.remove(keyboard);
    });
}

void SwipeHook::cancelKeyPress(QQuickItem *key) const
{
    if (!key) {
        return;
    }
    // WHY ungrab the key's touch area: KDE's KeyMouseArea (a MultiPointTouchArea) then reports the press as
    // cancelled, so the key types nothing and its repeat/press-and-hold timers stop; our layer keeps the finger.
    const auto areas = key->findChildren<QQuickItem *>();
    for (QQuickItem *area : areas) {
        if (area->inherits("QQuickMultiPointTouchArea")) {
            area->ungrabTouchPoints();
            area->ungrabMouse();
        }
    }
}

QObject *SwipeHook::controllerFor(QObject *anchor) const
{
    auto attachment = m_attached.value(keyboardRoot(anchor));
    return attachment ? attachment->controller() : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------

KeyboardAttachment::KeyboardAttachment(QQuickItem *keyboard, QQuickItem *loader, const QStringList &packages, QObject *parent)
    : QObject(parent)
    , m_keyboard(keyboard)
    , m_loader(loader)
    , m_packages(packages)
{
    // WHY queued: these fire while KDE's Loader swaps layouts; decorating mid-swap would touch a half-made item.
    connect(m_loader, SIGNAL(itemChanged()), this, SLOT(decorate()), Qt::QueuedConnection);
    connect(m_keyboard, SIGNAL(packageIdChanged()), this, SLOT(decorate()), Qt::QueuedConnection);
    connect(m_keyboard, SIGNAL(layoutTypeChanged()), this, SLOT(decorate()), Qt::QueuedConnection);
    const int engineSignal = m_keyboard->metaObject()->indexOfSignal("inputEngineChanged()");
    if (engineSignal >= 0) {
        connect(m_keyboard, SIGNAL(inputEngineChanged()), this, SLOT(rebuild()), Qt::QueuedConnection);
    }
    rebuild();
}

void KeyboardAttachment::rebuild()
{
    // WHY a fresh controller per engine: the editor and every session belong to one KDE input engine.
    undecorate();
    delete m_controller;
    delete m_editor;
    auto *engine = m_keyboard ? m_keyboard->property("inputEngine").value<QObject *>() : nullptr;
    if (!engine) {
        return;
    }
    auto *editor = new InputEngineEditor(engine, this);
    m_editor = editor;
    m_controller = new SwipeController(editor, this);
    decorate();
}

KeyboardAttachment::~KeyboardAttachment()
{
    undecorate();
}

QObject *KeyboardAttachment::controller() const
{
    return m_controller;
}

void KeyboardAttachment::addPackage(const QString &packageId)
{
    if (!packageId.isEmpty() && !m_packages.contains(packageId)) {
        m_packages << packageId;
    }
}

void KeyboardAttachment::undecorate()
{
    delete m_strip;
    delete m_layer;
    m_item = nullptr;
    m_column = nullptr;
}

void KeyboardAttachment::decorate()
{
    auto *item = m_loader ? qobject_cast<QQuickItem *>(m_loader->property("item").value<QObject *>()) : nullptr;
    const QString packageId = m_keyboard ? m_keyboard->property("packageId").toString() : QString();
    const bool ours = item && m_controller && m_packages.contains(packageId);
    // The column our row goes into: the layout itself (KeyboardLayout), or the page a KeyboardLayoutLoader shows
    // (KDE's symbol pages are Loaders with two pages).
    QQuickItem *column = item;
    if (item && item->metaObject()->indexOfProperty("panelHeightFactor") < 0) {
        column = qobject_cast<QQuickItem *>(item->property("item").value<QObject *>());
        if (item != m_watchedPages) {
            m_watchedPages = item;
            if (item->metaObject()->indexOfSignal("itemChanged()") >= 0) {
                connect(item, SIGNAL(itemChanged()), this, SLOT(decorate()), Qt::QueuedConnection);
            }
        }
    }
    if (item == m_item && column == m_column && m_strip && m_layer && ours) {
        return;
    }
    undecorate();
    if (!ours) {
        return; // WHY: a language we did not generate (Russian, Greek...) stays exactly KDE's
    }
    QQmlEngine *engine = qmlEngine(m_keyboard);
    if (!engine || !column || column->metaObject()->indexOfProperty("panelHeightFactor") < 0) {
        qCWarning(lcHook69) << "Swipe Keyboard: no KeyboardLayout column in this layout; leaving it stock";
        return;
    }
    QQmlComponent stripComponent(engine);
    stripComponent.loadFromModule("SwipeKeyboard69", "SuggestionStrip");
    QQmlComponent layerComponent(engine);
    layerComponent.loadFromModule("SwipeKeyboard69", "SwipeLayer");
    const QVariantMap common{{QStringLiteral("controller"), QVariant::fromValue(m_controller.data())}};
    auto *strip = qobject_cast<QQuickItem *>(stripComponent.createWithInitialProperties(common));
    QVariantMap layerProps = common;
    layerProps.insert(QStringLiteral("layoutItem"), QVariant::fromValue(item));
    layerProps.insert(QStringLiteral("keyboard"), QVariant::fromValue(m_keyboard.data()));
    layerProps.insert(QStringLiteral("popup"), QVariant::fromValue(findUp(m_keyboard, "alternativeKeysPopupItem")));
    auto *layer = qobject_cast<QQuickItem *>(layerComponent.createWithInitialProperties(layerProps));
    if (!strip || !layer) {
        qCWarning(lcHook69) << "Swipe Keyboard: could not create its row/layer:" << stripComponent.errorString() << layerComponent.errorString();
        delete strip;
        delete layer;
        return;
    }
    // Row 0 of the layout's column. WHY the panel keeps KDE's height (no panelHeightFactor change): symbol pages are
    // Loaders whose height factor cannot be raised, so a taller letters page would make the keyboard jump at every
    // "?123". The row takes its share from the key rows on every page instead.
    const auto children = column->childItems();
    strip->setParent(column);
    strip->setParentItem(column);
    if (!children.isEmpty()) {
        strip->stackBefore(children.first());
    }
    // The layer covers the keys (it is the Loader's child, so KDE's layout never treats it as a row).
    layer->setParent(m_loader);
    layer->setParentItem(m_loader);
    layer->setZ(10);
    m_item = item;
    m_column = column;
    m_strip = strip;
    m_layer = layer;
}
