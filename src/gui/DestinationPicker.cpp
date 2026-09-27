#include "gui/DestinationPicker.h"

#include <QQmlContext>
#include <QQmlError>
#include <QQuickWidget>
#include <QShowEvent>
#include <QUrl>
#include <QVBoxLayout>

#include "gui/qml/DestinationPickerModel.h"
#include "gui/qml/QmlTheme.h"

namespace gui {

// The application's own rows and sections: one data file, so the picker cannot be
// grouped by a table the exporter is not planning from.
DestinationPicker::DestinationPicker(GUIResources& aResources, const QString& aCurrentId,
                                     QWidget* aParent)
    : DestinationPicker(ctrl::ExportTargets::load(), ctrl::ExportTargets::loadGroups(),
                        aResources, aCurrentId, aParent) {
    // A destination table that cannot be read leaves an EMPTY picker, and an empty
    // picker says "this build knows no platforms" rather than "the table is broken".
    // The reasons are already written and translated (ExportTargets::load takes the
    // error out-parameter), so ask for one and state it in the list. The second read
    // costs a file parse, and only on this path: the normal path never gets here
    // because the table it loaded has rows.
    if (mModel && mModel->destinationCount() == 0) {
        QString error;
        ctrl::ExportTargets::load(ctrl::ExportTargets::defaultPath(), &error);
        if (!error.isEmpty()) mModel->setError(error);
    }
}

DestinationPicker::DestinationPicker(const QList<ctrl::ExportTarget>& aTargets,
                                     const QList<ctrl::ExportGroup>& aGroups,
                                     GUIResources& /*aResources*/, const QString& aCurrentId,
                                     QWidget* aParent)
    : QDialog(aParent) {
    // A native framed dialog: this is a dialog, not a floating popover, and the
    // window manager's own title bar is what a person expects to move it by.
    setObjectName(QString::fromUtf8("destinationPicker"));
    setModal(true);
    resize(560, 620);

    // The model owns every decision (grouping, expansion, filtering, selection).
    mModel = new DestinationPickerModel(aTargets, aGroups, aCurrentId, this);
    // The window title is the model's string, so the translation path the test
    // asserts is the one the window actually uses.
    setWindowTitle(mModel->windowTitleText());

    mTheme = new QmlTheme(this);

    mQuick = new QQuickWidget(this);
    mQuick->setResizeMode(QQuickWidget::SizeRootObjectToView);
    // The scene owns the text caret, so the widget has to be able to hold focus and
    // hand it to the QML item that wants it.
    mQuick->setFocusPolicy(Qt::StrongFocus);
    mQuick->rootContext()->setContextProperty(QStringLiteral("picker"), mModel);
    mQuick->rootContext()->setContextProperty(QStringLiteral("theme"), mTheme);
    mQuick->setSource(QUrl(QStringLiteral("qrc:/AnimeEffects/Dialogs/qml/DestinationPicker.qml")));
    // A scene that fails to load is a WHITE dialog, and the load is asynchronous:
    // status() is still Loading right after setSource(), so asking once here answers
    // nothing. Report the errors when they actually arrive, or a broken binding stays
    // a blank window nobody can diagnose.
    connect(mQuick, &QQuickWidget::statusChanged, this, [this](QQuickWidget::Status aStatus) {
        if (aStatus != QQuickWidget::Error) return;
        for (const QQmlError& e : mQuick->errors()) qWarning("%s", qPrintable(e.toString()));
    });

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(mQuick);

    // The model is where "this is the answer" and "this is cancel" are decided; the
    // shell only turns those into the QDialog result the call sites already read.
    connect(mModel, &DestinationPickerModel::accepted, this, [this]() {
        mChosenId = mModel->chosenId();
        accept();
    });
    connect(mModel, &DestinationPickerModel::rejected, this, &QDialog::reject);

    mQuick->setFocus();
}

DestinationPicker::~DestinationPicker() {
    // Tear down the QML scene before the model/theme context QObjects die, so the
    // scene's binding teardown never re-reads a destroyed context property.
    delete mQuick;
    mQuick = nullptr;
}

int DestinationPicker::visibleRowCount() const {
    return mModel ? mModel->visibleRowCount() : 0;
}

void DestinationPicker::showEvent(QShowEvent* aEvent) {
    QDialog::showEvent(aEvent);
    // A Qt Quick scene only has ACTIVE focus inside an ACTIVE window, and the scene
    // set its active focus item (the search field) when it was constructed - before
    // this dialog existed on screen. Claim the activation and the widget focus on
    // every show, or the field opens without a caret and every keystroke is lost.
    // The popup sibling needed exactly this on Windows; a dialog needs it wherever the
    // window manager does not activate the new window for us.
    activateWindow();
    mQuick->setFocus();
}

} // namespace gui
