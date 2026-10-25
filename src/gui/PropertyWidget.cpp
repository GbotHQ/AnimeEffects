#include "gui/PropertyWidget.h"

#include "gui/qml/InspectorViewModel.h"
#include "gui/qml/QmlTheme.h"
#include "gui/theme/Colors.h"

#include <QApplication>
#include <QQuickWidget>
#include <QQmlContext>
#include <QUrl>
#include <QVBoxLayout>

namespace gui {

namespace {
QFont uiFontFromLocale(const LocaleParam& aLocale) {
    QFont font(QApplication::font());
    if (!aLocale.fontFamily.isEmpty())
        font.setFamily(aLocale.fontFamily);
    const QString size = aLocale.fontSize.trimmed();
    bool ok = false;
    if (size.endsWith(QStringLiteral("pt"), Qt::CaseInsensitive)) {
        const qreal pt = size.left(size.size() - 2).toDouble(&ok);
        if (ok) font.setPointSizeF(pt);
    } else if (size.endsWith(QStringLiteral("px"), Qt::CaseInsensitive)) {
        const int px = size.left(size.size() - 2).toInt(&ok);
        if (ok) font.setPixelSize(px);
    } else if (!size.isEmpty()) {
        const qreal pt = size.toDouble(&ok);
        if (ok) font.setPointSizeF(pt);
    }
    return font;
}
} // namespace

PropertyWidget::PropertyWidget(ViaPoint& aViaPoint, QWidget* aParent, GUIResources* aGUIResources,
                               const LocaleParam& aLocale):
    QWidget(aParent),
    mProject(),
    mTimeLineSlot(),
    mNodeAttrSlot(),
    mResModifiedSlot(),
    mTreeRestructSlot(),
    mProjAttrSlot(),
    mModel(),
    mTheme(),
    mUiFont(uiFontFromLocale(aLocale)),
    mQuick() {
    Q_UNUSED(aGUIResources);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::MinimumExpanding);

    mModel = new InspectorViewModel(&aViaPoint, this);
    mTheme = new QmlTheme(this);

    // a filter row's "paint mask" reaches the window, which owns the toolbox
    mModel->onMaskPaintRequested.connect([this](core::TimeKeyType aLane, core::FilterMaskChannel aChannel) {
        onMaskPaintRequested(aLane, aChannel);
    });

    mQuick = new QQuickWidget(this);
    mQuick->setResizeMode(QQuickWidget::SizeRootObjectToView);
    mQuick->rootContext()->setContextProperty(QStringLiteral("inspector"), mModel);
    mQuick->rootContext()->setContextProperty(QStringLiteral("theme"), mTheme);
    // QML's Qt.application.font is the process font; the dock's font comes from
    // LocaleParam and is applied to widgets through standard.ssa, so the scene
    // needs that font explicitly to match 8pt Geist (or the locale override).
    mQuick->rootContext()->setContextProperty(QStringLiteral("uiFont"), mUiFont);
    mQuick->setClearColor(theme::Colors::current().base);
    mQuick->setSource(QUrl(QStringLiteral("qrc:/AnimeEffects/Dialogs/qml/PropertiesPanel.qml")));
    connect(mQuick, &QQuickWidget::statusChanged, this, [this](QQuickWidget::Status aStatus) {
        if (aStatus == QQuickWidget::Error) {
            for (const QQmlError& error : mQuick->errors())
                qWarning().noquote() << error.toString();
        }
    });

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(mQuick);
}

PropertyWidget::~PropertyWidget() {
    unlinkProject();
    // The scene binds to the model and theme context objects. Tear it down
    // first, as every dialog shell here does, so teardown never re-reads a
    // destroyed context property.
    delete mQuick;
    mQuick = nullptr;
}

void PropertyWidget::unlinkProject() {
    if (mProject) {
        mProject->onTimeLineModified.disconnect(mTimeLineSlot);
        mProject->onNodeAttributeModified.disconnect(mNodeAttrSlot);
        mProject->onResourceModified.disconnect(mResModifiedSlot);
        mProject->onTreeRestructured.disconnect(mTreeRestructSlot);
        mProject->onProjectAttributeModified.disconnect(mProjAttrSlot);
        mProject.reset();
    }
}

void PropertyWidget::setProject(core::Project* aProject) {
    unlinkProject();
    mModel->setProject(nullptr);

    if (aProject) {
        mProject = aProject->pointee();

        mTimeLineSlot = aProject->onTimeLineModified.connect(this, &PropertyWidget::onKeyUpdated);
        mNodeAttrSlot = aProject->onNodeAttributeModified.connect(this, &PropertyWidget::onAttributeUpdated);
        mResModifiedSlot =
            aProject->onResourceModified.connect([=](core::ResourceEvent&, bool) { updateAllProperties(); });
        mTreeRestructSlot =
            aProject->onTreeRestructured.connect([=](core::ObjectTreeEvent&, bool) { updateAllProperties(); });
        mProjAttrSlot =
            aProject->onProjectAttributeModified.connect([=](core::ProjectEvent&, bool) { mModel->refresh(); });

        mModel->setProject(aProject);
    }
}

void PropertyWidget::updateAllProperties() {
    mModel->refresh();
}

void PropertyWidget::onSelectionChanged(core::ObjectNode* aRepresentNode) { mModel->setTarget(aRepresentNode); }

void PropertyWidget::onAttributeUpdated(core::ObjectNode&, bool) {
    mModel->refreshAttributes();
    onVisualUpdated();
}

void PropertyWidget::onKeyUpdated(core::TimeLineEvent&, bool) { mModel->refreshKeys(); }

void PropertyWidget::onFrameUpdated() { mModel->refreshFrame(); }

void PropertyWidget::onPlayBackStateChanged(bool aIsActive) { mModel->setPlaybackActive(aIsActive); }

void PropertyWidget::refreshTheme() {
    mTheme->refresh();
    mModel->refreshTheme();
    mQuick->setClearColor(theme::Colors::current().base);
}

} // namespace gui
