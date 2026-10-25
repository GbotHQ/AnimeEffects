#include "gui/PropertyWidget.h"

#include "gui/qml/InspectorViewModel.h"
#include "gui/qml/QmlTheme.h"
#include "gui/theme/Colors.h"

#include <QQuickWidget>
#include <QQmlContext>
#include <QUrl>
#include <QVBoxLayout>

namespace gui {

PropertyWidget::PropertyWidget(ViaPoint& aViaPoint, QWidget* aParent, GUIResources* aGUIResources):
    QWidget(aParent),
    mProject(),
    mTimeLineSlot(),
    mNodeAttrSlot(),
    mResModifiedSlot(),
    mTreeRestructSlot(),
    mProjAttrSlot(),
    mModel(),
    mTheme(),
    mQuick() {
    Q_UNUSED(aGUIResources);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::MinimumExpanding);

    mModel = new InspectorViewModel(&aViaPoint, this);
    mTheme = new QmlTheme(this);

    mQuick = new QQuickWidget(this);
    mQuick->setResizeMode(QQuickWidget::SizeRootObjectToView);
    mQuick->rootContext()->setContextProperty(QStringLiteral("inspector"), mModel);
    mQuick->rootContext()->setContextProperty(QStringLiteral("theme"), mTheme);
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
