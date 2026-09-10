#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QRegularExpression>
#include "GeneralSettingDialog.h"
#include "util/IProgressReporter.h"
#include "gl/Global.h"
#include "ctrl/Exporter.h"
#include "gui/MainWindow.h"
#include "gui/DockSash.h"
#include "gui/ExportDialog.h"
#include "gui/NewProjectDialog.h"
#include "gui/ProjectHook.h"
#include "gui/menu/menu_ProgressReporter.h"
#include "util/NetworkUtil.h"
#include <set>
#include <utility>
#include <QComboBox>
// This thing is held by duct tape and OOP hell I swear...
#include "ctrl/ExportParams.h"

namespace {
class EventSuspender {
    QWriteLocker mRenderingLocker;

public:
    EventSuspender(gui::MainDisplayWidget& aMainDisplay, gui::TargetWidget& aTarget, bool aStopPlayback = true):
        mRenderingLocker(&aMainDisplay.renderingLock()) {
        // Manual saves stop playback so the serialized state is stable. The
        // autosave path keeps playback running: the frame timer is starved
        // for the duration of the save anyway (single UI thread), and
        // stopping would leave the animation paused until the user re-presses
        // play — a surprise on every autosave tick.
        if (aStopPlayback) {
            aTarget.stop();
        }
    }
};

} // namespace

namespace gui {

MainWindow::MainWindow(ctrl::System& aSystem, GUIResources& aResources, LocaleParam  aLocaleParam):
    QMainWindow(nullptr),
    autosaveTimer(nullptr),
    mSystem(aSystem),
    mGUIResources(aResources),
    mViaPoint(this),
    mMainMenuBar(),
    mMainDisplay(),
    mProjectTabBar(),
    mTarget(),
    mProperty(),
    mTool(),
    mResourceDialog(),
    mCurrent(),
    mLocaleParam(std::move(aLocaleParam)) {
    // setup default opengl format
    {
        QSurfaceFormat format;
        format.setVersion(gl::Global::kVersion.first, gl::Global::kVersion.second);
        format.setProfile(QSurfaceFormat::CoreProfile);
        format.setSamples(4);
        QSurfaceFormat::setDefaultFormat(format);
    }

    // setup UI
    {
        this->setObjectName(QStringLiteral("MainWindow"));
        this->setWindowIcon(QIcon("../src/AnimeEffects.ico"));
        this->setMouseTracking(true);
        this->setFocusPolicy(Qt::NoFocus);
        this->setAcceptDrops(false);
        this->setTabShape(QTabWidget::Rounded);
        this->setDockOptions(AnimatedDocks);
        this->setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    }

    // initialize via point
    {
        mViaPoint.setMainViewSetting(mMainViewSetting);

        mMouseSetting.load();
        mViaPoint.setMouseSetting(mMouseSetting);
    }

    // key binding
    {
        mKeyCommandMap.reset(new KeyCommandMap(*this));

        QSettings settings(
            QSettings::IniFormat,
            QSettings::UserScope,
            QApplication::organizationName(),
            QApplication::applicationName()
        );
        settings.beginGroup("keybindings");
        mKeyCommandMap->readFrom(settings);
        settings.endGroup();

        mViaPoint.setKeyCommandMap(mKeyCommandMap.data());

        mKeyCommandInvoker.reset(new KeyCommandInvoker(*mKeyCommandMap));
        mViaPoint.setKeyCommandInvoker(mKeyCommandInvoker.data());
    }

    // create main menu bar
    {
        mMainMenuBar = new MainMenuBar(*this, mViaPoint, mGUIResources, this);
        mViaPoint.setMainMenuBar(mMainMenuBar);
        this->setMenuBar(mMainMenuBar);
    }
    // initialize timer
    {
        timeElapsed.start();
        lastPress = 0;
        lastRelease = 0;
    }

    // create main display
    {
#if defined(Q_OS_WIN)
        constexpr float fontScale = 1.5f;
#else
        const float fontScale = 1.3f;
#endif
        auto font = this->font();
        if (font.pixelSize() > 0)
            font.setPixelSize(font.pixelSize() * static_cast<int>(fontScale));
        else
            font.setPointSizeF(font.pointSizeF() * fontScale);
        mMainDisplayStyle.reset(new MainDisplayStyle(font, mGUIResources));
        mMainDisplay = new MainDisplayWidget(mViaPoint, this);
        this->setCentralWidget(mMainDisplay);
        mMainDisplay->setViewportBackground(mGUIResources.viewportBackground());

        mProjectTabBar = new ProjectTabBar(mMainDisplay, mGUIResources);
        mMainDisplay->setProjectTabBar(mProjectTabBar);
        mProjectTabBar->onCurrentChanged.connect(this, &MainWindow::onProjectTabChanged);
        mProjectTabBar->onSaveTab.connect(this, &MainWindow::onTabSaveRequested);
        mProjectTabBar->onSaveTabAs.connect(this, &MainWindow::onTabSaveAsRequested);
        mProjectTabBar->onCloseTab.connect(this, &MainWindow::onTabCloseRequested);
        mProjectTabBar->onCloseOtherTabs.connect(this, &MainWindow::onTabCloseOthersRequested);
        mProjectTabBar->onCloseAllTabs.connect(this, &MainWindow::onTabCloseAllRequested);
        mProjectTabBar->onCopyPath.connect(this, &MainWindow::onTabCopyPathRequested);
    }

    // create targeting widget
    {
        auto* dockWidget = new QDockWidget(this);
        dockWidget->setObjectName("animationDock");
        dockWidget->setWindowTitle(tr("Animation"));
        dockWidget->setFeatures(QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetMovable);
        dockWidget->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
        this->addDockWidget(Qt::BottomDockWidgetArea, dockWidget);

        mTarget = new TargetWidget(mViaPoint, mGUIResources, dockWidget, QSize(256, 256));
        dockWidget->setWidget(mTarget);
    }

    // create property widget
    {
        auto* dockWidget = new QDockWidget(this);
        dockWidget->setObjectName("propertiesDock");
        dockWidget->setWindowTitle(tr("Properties"));
        dockWidget->setFeatures(QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetMovable);
        dockWidget->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        this->addDockWidget(Qt::RightDockWidgetArea, dockWidget);

        mDockPropertyWidget = dockWidget;

        // Panel width budget: min = default = 320 (everything fits; the
        // fields cap at 70% of the column), max = 2x min - a wider dock
        // would just stretch empty space.
        dockWidget->setMinimumWidth(320);
        dockWidget->setMaximumWidth(640);

#if 0
        mProperty = new PropertyWidget(mViaPoint, splitter, &mGUIResources);
        dockWidget->setWidget(mProperty);
#else
        auto splitter = new gui::ThinSplitter(Qt::Vertical, dockWidget);
        splitter->setObjectName("propertysplitter");
        dockWidget->setWidget(splitter);

        mProperty = new PropertyWidget(mViaPoint, splitter, &mGUIResources);
        splitter->addWidget(mProperty);

        auto textEdit = new QPlainTextEdit(splitter);
        textEdit->setUndoRedoEnabled(false);
        textEdit->setMaximumBlockCount(32);
        textEdit->setReadOnly(true);
        mViaPoint.setLogView(textEdit);

        splitter->addWidget(textEdit);
        splitter->setCollapsible(0, false);
        splitter->setCollapsible(1, false);


        QList<int> list;
        list.append(9000);
        list.append(1000);
        splitter->setSizes(list);
#endif
    }

    // create tool widget
    {
        auto* dockWidget = new QDockWidget(this);
        dockWidget->setObjectName("toolsDock");
        dockWidget->setWindowTitle(tr("Tools"));
        dockWidget->setFeatures(QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetMovable);
        this->addDockWidget(Qt::LeftDockWidgetArea, dockWidget);

        mDockToolWidget = dockWidget;

        mTool = new ToolWidget(mViaPoint, mGUIResources, *mKeyCommandMap, QSize(172, 136), dockWidget);
        dockWidget->setWidget(mTool);
    }

    // create resource dialog
    {
        mResourceDialog = new ResourceDialog(mViaPoint, false, this);
        mViaPoint.setResourceDialog(mResourceDialog);
    }

    // dock-edge sashes: 12px invisible hitboxes + 1px hairlines over the
    // zero-width native separators (see standard.ssa); resizes docks through
    // QMainWindow::resizeDocks
    new gui::DockSash(this);

    // create driver holder
    { mDriverHolder.reset(new DriverHolder(mViaPoint)); }

    // connection
    /// @note Maybe a sequence of connections is meaningful.
    {
        DriverHolder& driver = *mDriverHolder;
        MainDisplayWidget& disp = *mMainDisplay;
        PropertyWidget& prop = *mProperty;
        ToolWidget& tool = *mTool;
        ObjectTreeWidget& objTree = mTarget->objectTreeWidget();
        TimeLineWidget& timeLine = mTarget->timeLineWidget();
        MainMenuBar& menu = *mMainMenuBar;
        ViaPoint& via = mViaPoint;

        driver.onVisualUpdated.connect(&disp, &MainDisplayWidget::onVisualUpdated);
        tool.onVisualUpdated.connect(&disp, &MainDisplayWidget::onVisualUpdated);
        prop.onVisualUpdated.connect(&disp, &MainDisplayWidget::onVisualUpdated);
        objTree.onVisibilityUpdated.connect(&disp, &MainDisplayWidget::onVisualUpdated);
        menu.onVisualUpdated.connect(&disp, &MainDisplayWidget::onVisualUpdated);
        via.onVisualUpdated.connect(&disp, &MainDisplayWidget::onVisualUpdated);

        tool.onToolChanged.connect(&disp, &MainDisplayWidget::onToolChanged);
        tool.onFinalizeTool.connect(&disp, &MainDisplayWidget::onFinalizeTool);
        tool.onViewSettingChanged.connect(&disp, &MainDisplayWidget::onViewSettingChanged);

        objTree.onSelectionChanged.connect(&driver, &DriverHolder::onSelectionChanged);
        objTree.onSelectionChanged.connect(mProperty, &PropertyWidget::onSelectionChanged);
        objTree.onSelectionChanged.connect(&timeLine, &TimeLineWidget::onSelectionChanged);

        objTree.onTreeViewUpdated.connect(&timeLine, &TimeLineWidget::onTreeViewUpdated);
        objTree.onScrollUpdated.connect(&timeLine, &TimeLineWidget::onScrollUpdated);
        timeLine.onVerticalScrollRequested.connect(
            [&objTree](int aValue) { objTree.verticalScrollBar()->setValue(aValue); });

        timeLine.onFrameUpdated.connect(&driver, &DriverHolder::onFrameUpdated);
        timeLine.onFrameUpdated.connect(&prop, &PropertyWidget::onFrameUpdated);
        timeLine.onPlayBackStateChanged.connect(&prop, &PropertyWidget::onPlayBackStateChanged);

        menu.onProjectAttributeUpdated.connect(&disp, &MainDisplayWidget::onProjectAttributeUpdated);
        menu.onProjectAttributeUpdated.connect(&timeLine, &TimeLineWidget::onProjectAttributeUpdated);
        menu.onProjectAttributeUpdated.connect(&driver, &DriverHolder::onProjectAttributeUpdated);
        menu.onTimeFormatChanged.connect(&timeLine, &TimeLineWidget::triggerOnTimeFormatChanged);

        mGUIResources.onThemeChanged.connect(this, &MainWindow::onThemeUpdated);

        mSystem.setAnimator(*mTarget);
    }

    this->setFocusPolicy(Qt::StrongFocus);

#if 0
    auto scUndo = new QShortcut(QKeySequence("Ctrl+Z"), this);
    auto scRedo = new QShortcut(QKeySequence("Ctrl+Shift+Z"), this);
    //scUndo->setContext(Qt::WidgetWithChildrenShortcut);
    //scRedo->setContext(Qt::WidgetWithChildrenShortcut);
    scUndo->setContext(Qt::ApplicationShortcut);
    scRedo->setContext(Qt::ApplicationShortcut);

    this->connect(scUndo, &QShortcut::activated, [=](){ this->onUndoTriggered(); });
    this->connect(scRedo, &QShortcut::activated, [=](){ this->onRedoTriggered(); });
#else
    {
        auto key = mKeyCommandMap->get("Undo");
        if (key)
            key->addListener([=]() { this->onUndoTriggered(); });
    }
    {
        auto key = mKeyCommandMap->get("Redo");
        if (key)
            key->addListener([=]() { this->onRedoTriggered(); });
    }
    {
        auto key = mKeyCommandMap->get("SaveProject");
        if (key)
            key->addListener([=]() { this->onSaveProjectTriggered(); });
    }
    {
        auto key = mKeyCommandMap->get("PlayPause");
        if (key)
            key->addListener([=]() { this->onPlayPauseTriggered(); });
    }
    {
        auto key = mKeyCommandMap->get("ToggleDocks");
        if (key)
            key->addListener([=]() { this->onDockToggle(); });
    }
    {
        auto key = mKeyCommandMap->get("ToggleRepeat");
        if (key)
            key->addListener([=]() { this->onLoopToggle(); });
    }
    {
        auto key = mKeyCommandMap->get("MoveRight");
        if (key)
            key->addListener([=]() { this->onDisplacementTriggered(1); });
    }
    {
        auto key = mKeyCommandMap->get("MoveLeft");
        if (key)
            key->addListener([=]() { this->onDisplacementTriggered(-1); });
    }
    {
        auto key = mKeyCommandMap->get("MoveToInit");
        if (key)
            key->addListener([=]() { this->onMovementTriggered("Init"); });
    }
    {
        auto key = mKeyCommandMap->get("MoveToLast");
        if (key)
            key->addListener([=]() { this->onMovementTriggered("Last"); });
    }
    {
        auto key = mKeyCommandMap->get("MoveFrameLeft");
        if(key)
            key->addListener([=]() {this->onMoveFrameTriggered(false);});
    }
    {
        auto key = mKeyCommandMap->get("MoveFrameRight");
        if(key)
            key->addListener([=]() {this->onMoveFrameTriggered(true);});
    }
#endif

    // autosave
    // The settings dialog applies autosave changes live through
    // onGeneralSettingsApplied; the timer is (re)configured centrally here.
    mViaPoint.onGeneralSettingsChanged.connect(this, &MainWindow::onGeneralSettingsApplied);
    updateAutosaveState();
}

MainWindow::~MainWindow() { closeAllProjects(); }

void MainWindow::onGeneralSettingsApplied() { updateAutosaveState(); }

void MainWindow::updateAutosaveState() {
    QSettings settings;
    // Missing key defaults ON, matching the GeneralSettingsDialog checkbox.
    const QVariant enabledVar = settings.value("generalsettings/projects/enableAutosave");
    const bool enabled = !enabledVar.isValid() || enabledVar.toBool();
    const QVariant delayVar = settings.value("generalsettings/projects/autosaveDelay");
    int delayMin = delayVar.isValid() ? delayVar.toInt() : 5;
    if (delayMin < 1) {
        delayMin = 1; // 0 would arm a continuously-firing timer
    }

    if (enabled) {
        if (!autosaveTimer) {
            autosaveTimer = new QTimer(this);
            connect(autosaveTimer, SIGNAL(timeout()), this, SLOT(autoSave()));
        }
        // start() arms a fresh countdown; on a running timer it restarts it.
        autosaveTimer->start(delayMin * 60000);
    } else if (autosaveTimer) {
        autosaveTimer->stop();
    }
}

void MainWindow::autoSave() {
    if (!autosaveTimer) {
        return;
    }

    QSettings settings;
    const QVariant enabledVar = settings.value("generalsettings/projects/enableAutosave");
    if (!enabledVar.isValid() || !enabledVar.toBool()) {
        autosaveTimer->stop();
        return;
    }

    const QVariant delayVar = settings.value("generalsettings/projects/autosaveDelay");
    int autoDelay = delayVar.isValid() ? delayVar.toInt() : 5;
    if (autoDelay < 1) {
        autoDelay = 1;
    }
    // Re-arm so the next autosave is a full interval after this one, not
    // relative to app launch or the last manual save.
    autosaveTimer->start(autoDelay * 60000);

    // Save every modified project — not just the active tab. Background tabs
    // otherwise never get autosaved, losing work on a crash.
    for (int i = 0; i < mSystem.projectCount(); ++i) {
        core::Project* project = mSystem.project(i);
        if (!project || !project->isModified()) {
            continue;
        }

        if (project->isNameless()) {
            // Never-saved projects get a recovery copy in the app cache. The
            // project stays unsaved (dirty flag and Save As prompt remain);
            // the log line names the file so it can be reopened after a crash.
            EventSuspender suspender(*mMainDisplay, *mTarget, false);
            QString recoveryPath;
            auto result = mSystem.saveRecoveryCopy(*project, recoveryPath);
            if (result) {
                mViaPoint.pushLog("Automatically saved unsaved project to: " + recoveryPath, ctrl::UILogType_Info);
            } else {
                mViaPoint.pushLog(
                    "Failed to automatically save unsaved project: " + result.message, ctrl::UILogType_Warn
                );
            }
        } else {
            const QString label = QFileInfo(project->fileName()).fileName();
            if (processProjectSaving(*project, false, true, false)) {
                mViaPoint.pushLog("Automatically saved project: " + label, ctrl::UILogType_Info);
            } else {
                mViaPoint.pushLog("Failed to automatically save project: " + label, ctrl::UILogType_Warn);
            }
        }
    }
}

void MainWindow::showWithSettings() {
#if defined(QT_NO_DEBUG) || 1
    QSettings settings;
    auto winSize = settings.value("mainwindow/size");
    auto isMax = settings.value("mainwindow/ismaximized");

    if (winSize.isValid()) {
        this->resize(winSize.toSize());
    }

    if (!isMax.isValid() || isMax.toBool()) {
        this->showMaximized();
    } else {
        this->show();
    }
#else
    this->showMaximized();
#endif

    // Offer crash-recovery of unsaved projects from a previous session once
    // the window is up; the prompt needs a visible parent.
    QTimer::singleShot(0, this, [this]() { offerRecoveryCopies(); });
}

void MainWindow::saveCurrentSettings(int aResultCode) {
#if defined(QT_NO_DEBUG) || 1
    if (aResultCode == 0) {
        QSettings settings;
        settings.setValue("mainwindow/ismaximized", this->isMaximized());
        if (!this->isMaximized()) {
            settings.setValue("mainwindow/size", this->size());
        }
    }
#else
    (void)aResultCode;
#endif
}

void MainWindow::testNewProject(const QString& aFilePath) {
    resetProjectRefs(nullptr);

    menu::ProgressReporter progress(false, this);

    core::Project::Attribute attribute;
    auto result = mSystem.newProject(aFilePath, attribute, new ProjectHook(), progress, false);

    if (result) {
        resetProjectRefs(result.project);
        mProjectTabBar->pushProject(*result.project);

        mMainDisplay->resetCamera();
    }
}

void MainWindow::closeAllProjects() {
    mProjectTabBar->removeAllProject();
    resetProjectRefs(nullptr);
    mSystem.closeAllProjects();
}

void MainWindow::resetProjectRefs(core::Project* aProject) {
    mCurrent = aProject;

    /// @note Maybe a sequence of connections is meaningful.

    if (aProject) {
        mDriverHolder->create(*aProject, *mMainDisplayStyle);
    } else {
        mDriverHolder->destroy();
    }

    mMainMenuBar->setProject(aProject);
    mMainDisplay->setProject(aProject);
    mTarget->setProject(aProject);
    mProperty->setProject(aProject);
    mResourceDialog->setProject(aProject);
    mViaPoint.setProject(aProject);

    mMainDisplay->setDriver(mDriverHolder->driver());
    mTool->setDriver(mDriverHolder->driver());
}

void MainWindow::onProjectTabChanged(core::Project& aProject) { resetProjectRefs(&aProject); }

void MainWindow::onThemeUpdated(theme::Theme& aTheme) {
    mMainDisplay->setViewportBackground(mGUIResources.viewportBackground());

    const QString standard = aTheme.loadStylesheet("standard.ssa");
    if (!standard.isEmpty()) {
        QString fontOption;
        {
            auto hasFamily = !mLocaleParam.fontFamily.isEmpty();
            auto hasSize = !mLocaleParam.fontSize.isEmpty();
            fontOption = "QWidget {" + (hasFamily ? ("font-family: " + mLocaleParam.fontFamily + ";") : "") +
                (hasSize ? ("font-size: " + mLocaleParam.fontSize + ";") : "") + " }\n";
            // top-level section headers step up one point from the base
            // font (QSS cannot express a relative size, so derive it from
            // the locale's pt value; the rule lives here so it tracks the
            // base font across locale/settings changes)
            if (hasSize) {
                const QString size = mLocaleParam.fontSize.trimmed();
                if (size.endsWith("pt", Qt::CaseInsensitive)) {
                    bool ok = false;
                    const qreal pt = size.left(size.size() - 2).toDouble(&ok);
                    if (ok) {
                        fontOption += QString("QAbstractButton#propertyPanelHeader { font-size: %1pt; }\n")
                                          .arg(QString::number(pt + 1.0, 'f', 0));
                    }
                }
            }
        }

        this->setStyleSheet(fontOption + standard);
    }

    mDockPropertyWidget->setStyleSheet(aTheme.loadStylesheet("propertywidget.ssa"));
    mDockToolWidget->setStyleSheet(aTheme.loadStylesheet("toolwidget.ssa"));

    if (!onStartup && !themeChangeWarned) {
        QMessageBox visualArtifactWarning;
        visualArtifactWarning.setWindowTitle(tr("Theme changed"));
        visualArtifactWarning.setText(
            tr("There may be visual artifacts after changing themes, we recommend you restart "
               "the application.")
        );
        visualArtifactWarning.setDefaultButton(QMessageBox::Ok);
        visualArtifactWarning.exec();
        themeChangeWarned = true;
    }
    else{
        onStartup = false;
    }
}

#if 0
void MainWindow::keyPressEvent(QKeyEvent* aEvent)
{
    //qDebug() << "mainwindow: input key =" << aEvent->key() << "text =" << aEvent->text();
    bool shouldUpdate = false;

    if (mSystem.project())
    {        
        if(aEvent->key() == Qt::Key_Z)
        {
            if (aEvent->modifiers().testFlag(Qt::ControlModifier))
            {
                if (aEvent->modifiers().testFlag(Qt::ShiftModifier))
                {
                    mSystem.project()->commandStack().redo();
                    shouldUpdate = true;
                    qDebug() << "redo";
                }
                else
                {
                    mSystem.project()->commandStack().undo();
                    shouldUpdate = true;
                    qDebug() << "undo";
                }
            }
        }
    }

    if (shouldUpdate)
    {
        mMainDisplay->updateRender();
    }
    /*
    else
    {
        mMainDisplay->throwKeyPress(aEvent);
    }
    */
}
#endif

QList<QString> disallowedRepeats = {"MoveCanvas", "RotateCanvas", "SaveProject"};

void MainWindow::keyPressEvent(QKeyEvent* aEvent) {
    // qDebug() << "input key =" << aEvent->key() << "text =" << aEvent->text();
    if (aEvent->isAutoRepeat()) {
        for (auto command : mKeyCommandMap->commands()) {
            if (disallowedRepeats.contains(command->key)) {
                const ctrl::KeyBinding keyBind(aEvent->key(), aEvent->modifiers());
                if (command->binding.matchesExactlyWith(keyBind)) {
                    return;
                }
            }
        }
        // Default delay of 125ms
        QSettings settings;
        auto delay = settings.value("generalsettings/keybindings/keyDelay");
        qint64 delayInMs = delay.isValid() ? delay.toInt() : 125;
        if (lastPress + delayInMs < timeElapsed.elapsed()) {
            lastPress = timeElapsed.elapsed();
            mKeyCommandInvoker->onKeyPressed(aEvent);
            QMainWindow::keyPressEvent(aEvent);
        } else {
            // qDebug() << "Time elapsed is :" << (lastPress+500)-timeElapsed.elapsed();
            return;
        }
    } else {
        mKeyCommandInvoker->onKeyPressed(aEvent);
        QMainWindow::keyPressEvent(aEvent);
    }
}

void MainWindow::keyReleaseEvent(QKeyEvent* aEvent) {
    // qDebug() << "release key =" << aEvent->key() << "text =" << aEvent->text();
    if (aEvent->isAutoRepeat()) {
        for (auto command : mKeyCommandMap->commands()) {
            if (disallowedRepeats.contains(command->key)) {
                const ctrl::KeyBinding keyBind(aEvent->key(), aEvent->modifiers());
                if (command->binding.matchesExactlyWith(keyBind)) {
                    return;
                }
            }
        }
        // Default delay of 125ms
        QSettings settings;
        auto delay = settings.value("generalsettings/keybindings/keyDelay");
        qint64 delayInMs = delay.isValid() ? delay.toInt() : 125;
        if (lastRelease + delayInMs < timeElapsed.elapsed()) {
            lastRelease = timeElapsed.elapsed();
            mKeyCommandInvoker->onKeyReleased(aEvent);
            QMainWindow::keyPressEvent(aEvent);
        } else {
            // qDebug() << "time elapsed is :" << (lastPress+500)-timeElapsed.elapsed();
            return;
        }
    } else {
        mKeyCommandInvoker->onKeyReleased(aEvent);
        QMainWindow::keyReleaseEvent(aEvent);
    }
}

void MainWindow::closeEvent(QCloseEvent* aEvent) {
    QVector<core::Project*> projects;
    for (int i = 0; i < mSystem.projectCount(); ++i) {
        auto* project = mSystem.project(i);
        if (project) {
            projects.push_back(project);
        }
    }
    if (!closeProjects(projects)) {
        aEvent->ignore();
        return;
    }
    aEvent->accept();
}

bool MainWindow::closeProjects(const QVector<core::Project*>& aProjects) {
    bool anyModified = false;
    for (auto* project : aProjects) {
        if (project && project->isModified()) {
            anyModified = true;
            break;
        }
    }

    if (anyModified) {
        const int result = confirmProjectClosing(aProjects);
        if (result == QMessageBox::Cancel) {
            return false;
        }
        if (result == QMessageBox::Yes) {
            for (auto* project : aProjects) {
                if (project && project->isModified() && !processProjectSaving(*project)) {
                    return false;
                }
            }
        }
    }

    for (auto* project : aProjects) {
        if (project) {
            closeProjectTab(*project);
        }
    }
    return true;
}

void MainWindow::closeProjectTab(core::Project& aProject) {
    // Sneaky potential crash
    if (QFileSystemWatcher* watcher = getWatcher()) {
        // imageTrees() is a std::list, so its count is unsigned (size_type);
        // the index has to be that same unsigned type. It starts at 0 and
        // only counts up, so an unsigned index cannot wrap here, while the
        // old `int x` would have overflowed on a list longer than INT_MAX.
        for (size_t x = 0; x < aProject.resourceHolder().imageTrees().size(); x += 1) {
            if (watcher->files().contains(aProject.resourceHolder().findAbsoluteFilePath(
                    *aProject.resourceHolder().imageTree(x).topNode
                ))) {
                watcher->removePath(aProject.resourceHolder().findAbsoluteFilePath(
                    *aProject.resourceHolder().imageTree(x).topNode
                ));
            }
        }
    }

    // Only a closed ACTIVE project hands its playback state to the next tab
    // (continuity when closing what you're working on). Closing an inactive tab
    // must not overwrite the active project's mediaPlayer/config.
    const bool wasCurrent = (mProjectTabBar->currentProject() == &aProject);

    const auto playerBackup = *aProject.mediaPlayer;
    const auto configBackup = *aProject.pConf;

    const auto closeProject = &aProject;
    mProjectTabBar->removeProject(*closeProject);
    resetProjectRefs(nullptr); ///@note update mCurrent
    // A closed tab's autosave copy is stale — saved projects already cleared
    // it, discarded ones should not linger as litter in the app cache.
    mSystem.clearRecoveryCopy(*closeProject);
    mSystem.closeProject(*closeProject);

    if (mProjectTabBar->currentProject()) {
        if (wasCurrent) {
            mProjectTabBar->currentProject()->mediaPlayer = new mediaState(playerBackup);
            mProjectTabBar->currentProject()->pConf = new std::vector(configBackup);
        }
        resetProjectRefs(mProjectTabBar->currentProject());
    }
}

int MainWindow::confirmProjectClosing(const QVector<core::Project*>& aProjects) {
    // Scoped to the close-set: a single modified project is named, several use
    // the generic message. Matches the save loop in closeProjects, which also
    // only touches aProjects.
    QString singleName;
    bool found = false;
    for (auto* project : aProjects) {
        if (!project || !project->isModified()) {
            continue;
        }
        if (found) {
            singleName.clear();
            break;
        }
        singleName = mProjectTabBar->getTabName(*project);
        found = true;
    }

    if (!singleName.isEmpty()) {
        return confirmProjectClosing(singleName + tr(" has been modified. Save changes?"));
    }
    return confirmProjectClosing(tr("Some projects have been modified. Save changes?"));
}

int MainWindow::confirmProjectClosing(const QString& aMessage) {
    QMessageBox msgBox;

    msgBox.setText(aMessage);

    msgBox.addButton(tr("Save Changes"), QMessageBox::YesRole);
    msgBox.addButton(tr("Discard Changes"), QMessageBox::NoRole);
    auto cancel = msgBox.addButton(tr("Cancel"), QMessageBox::RejectRole);
    msgBox.setDefaultButton(cancel);
    msgBox.exec();
    auto clicked = msgBox.clickedButton();
    if (clicked) {
        auto role = msgBox.buttonRole(clicked);
        if (role == QMessageBox::YesRole)
            return QMessageBox::Yes;
        else if (role == QMessageBox::NoRole)
            return QMessageBox::No;
        else if (role == QMessageBox::RejectRole)
            return QMessageBox::Cancel;
    }
    return QMessageBox::Cancel;
}

void MainWindow::onUndoTriggered() {
    if (mCurrent) {
        bool undone = false;
        auto ret = mCurrent->commandStack().undo(&undone);
        if (undone) {
            mViaPoint.pushUndoneLog(tr("Undone : ") + ret);
            mMainDisplay->updateRender();
        }
    }
}

void MainWindow::onDisplacementTriggered(int frameDisplacement) {
    mTarget->timeLineWidget().setFrame(mTarget->currentFrame().added(frameDisplacement));
}

void MainWindow::onPlayPauseTriggered() {
    mTarget->playBackWidget().PlayPause(); // If it's not playing then it will begin playback
}

void MainWindow::onMovementTriggered(const QString& frameMovement) {
    if (frameMovement == "Last") {
        // Land on the effective playback range's end (the temporary window's
        // end while it overrides) — the project range's max would fall short
        // when the window extends past it.
        mTarget->timeLineWidget().setFrame(core::Frame(mTarget->timeLineWidget().playbackRange().max()));
    } else {
        // Land on the effective playback range's start (the temporary
        // window's start while it overrides); the playhead clamps into it.
        mTarget->timeLineWidget().setFrame(core::Frame(mTarget->timeLineWidget().playbackRange().min()));
    }
}

void MainWindow::onDockToggle() // For some reason it does not work without this travesty
{
    if (mDockPropertyWidget->isHidden()) {
        mDockPropertyWidget->show();
    } else if (!mDockPropertyWidget->isHidden()) {
        mDockPropertyWidget->hide();
    }

    if (mDockToolWidget->isHidden()) {
        mDockToolWidget->show();
    } else if (!mDockToolWidget->isHidden()) {
        mDockToolWidget->hide();
    }

    if (mTarget->isHidden()) {
        mTarget->show();
    } else if (!mTarget->isHidden()) {
        mTarget->hide();
    }
}

void MainWindow::onLoopToggle() {
    if (mTarget->playBackWidget().isLoopChecked()) {
        mTarget->timeLineWidget().setPlayBackLoop(false);
        mTarget->playBackWidget().checkLoop(false);
    } else {
        mTarget->timeLineWidget().setPlayBackLoop(true);
        mTarget->playBackWidget().checkLoop(true);
    }
}

void MainWindow::onRedoTriggered() {
    if (mCurrent) {
        bool redone = false;
        auto ret = mCurrent->commandStack().redo(&redone);
        if (redone) {
            mViaPoint.pushRedoneLog(tr("Redone : ") + ret);
            mMainDisplay->updateRender();
        }
    }
}

void MainWindow::onNewProjectTriggered() {
    // stop animation and main display rendering
    EventSuspender suspender(*mMainDisplay, *mTarget);

    // input attribute
    core::Project::Attribute attribute;
    QString fileName;
    bool specifiesCanvasSize = false;
    {
        QScopedPointer<NewProjectDialog> dialog(new NewProjectDialog(this));

        dialog->exec();
        if (dialog->result() != QDialog::Accepted) {
            return;
        }
        attribute = dialog->attribute();
        fileName = dialog->fileName();
        specifiesCanvasSize = dialog->specifiesCanvasSize();
    }

    // clear old project
    resetProjectRefs(nullptr);

    // try loading
    ctrl::System::LoadResult result;
    {
        menu::ProgressReporter progress(false, this);

        result = mSystem.newProject(fileName, attribute, new ProjectHook(), progress, specifiesCanvasSize);
    }

    if (result.project) {
        resetProjectRefs(result.project);
        mProjectTabBar->pushProject(*result.project);

        mMainDisplay->resetCamera();
    } else {
        QMessageBox::warning(nullptr, tr("Loading Error"), result.messages());

        if (mProjectTabBar->currentProject()) {
            resetProjectRefs(mProjectTabBar->currentProject());
        }
    }
}

QFileSystemWatcher* globalWatcher = new QFileSystemWatcher();

QFileSystemWatcher* MainWindow::getWatcher() { return globalWatcher; }

void MainWindow::showInfoPopup(
    const QString& aTitle, const QString& aDetailText, const QString& aIcon, const QString& aDetailed
) {
    QMessageBox box;
    if (aIcon == "Info") {
        box.setIcon(QMessageBox::Information);
    } else if (aIcon == "Warn") {
        box.setIcon(QMessageBox::Warning);
    } else if (aIcon == "Critical") {
        box.setIcon(QMessageBox::Critical);
    }
    box.setStandardButtons(QMessageBox::Ok);
    box.setDefaultButton(QMessageBox::Ok);
    box.setWindowTitle(aTitle);
    box.setText(aDetailText);
    if (aDetailed != "nullptr") {
        box.setDetailedText(aDetailed);
    }
    box.exec();
}

void MainWindow::onOpenProjectTriggered() {
    // stop animation and main display rendering
    EventSuspender suspender(*mMainDisplay, *mTarget);

    QString fileName = QFileDialog::getOpenFileName(this, tr("Open File"), "", "ProjectFile (*.anie)");
    if (fileName.isEmpty())
        return;

    // clear old project
    resetProjectRefs(nullptr);

    // try loading
    ctrl::System::LoadResult result;
    {
        menu::ProgressReporter progress(false, this);
        result = mSystem.openProject(fileName, new ProjectHook(), progress);
    }

    if (result) {
        resetProjectRefs(result.project);
        mProjectTabBar->pushProject(*result.project);

        mMainDisplay->resetCamera();
        mDriverHolder->driver()->updateFrame();
        mDriverHolder->driver()->updateProjectAttribute();

    } else {
        QMessageBox::warning(nullptr, tr("Loading Error"), result.messages());

        if (mProjectTabBar->currentProject()) {
            resetProjectRefs(mProjectTabBar->currentProject());
        }
    }
}

void MainWindow::onOpenRecentTriggered(QString aFileName) {
    // stop animation and main display rendering
    EventSuspender suspender(*mMainDisplay, *mTarget);

    QString fileName = std::move(aFileName);

    // clear old project
    resetProjectRefs(nullptr);

    // try loading
    ctrl::System::LoadResult result;
    {
        menu::ProgressReporter progress(false, this);
        result = mSystem.openProject(fileName, new ProjectHook(), progress);
    }

    if (result) {
        resetProjectRefs(result.project);
        mProjectTabBar->pushProject(*result.project);
        mMainDisplay->resetCamera();
    } else {
        QMessageBox::warning(nullptr, tr("Loading Error"), result.messages());

        if (mProjectTabBar->currentProject()) {
            resetProjectRefs(mProjectTabBar->currentProject());
        }
    }
}

void MainWindow::offerRecoveryCopies() {
    // Drop incomplete recovery writes first so they are never offered or
    // re-swept each launch; nothing here is a real project file.
    mSystem.sweepStaleRecoveryTmp();
    const QStringList paths = mSystem.recoveryCopyPaths();
    if (paths.isEmpty()) {
        return;
    }

    for (const QString& path : paths) {
        const auto answer = QMessageBox::question(
            this,
            tr("Recover unsaved project?"),
            tr("AnimeEffects found an unsaved project from a previous session:\n%1\n\nOpen it?").arg(
                QDir::toNativeSeparators(path)
            ),
            QMessageBox::Open | QMessageBox::Discard,
            QMessageBox::Open
        );
        if (answer == QMessageBox::Open) {
            openRecoveryCopy(path);
        } else if (answer == QMessageBox::Discard) {
            // Only an explicit Discard deletes the copy. Closing the prompt
            // (window X / Alt-F4) yields NoButton, and that must leave the
            // file so the work is still offered at the next launch.
            mSystem.removeRecoveryCopy(path);
        }
    }
}

void MainWindow::openRecoveryCopy(const QString& aFilePath) {
    // stop animation and main display rendering
    EventSuspender suspender(*mMainDisplay, *mTarget);

    // clear old project
    resetProjectRefs(nullptr);

    // try loading
    ctrl::System::LoadResult result;
    {
        menu::ProgressReporter progress(false, this);
        result = mSystem.openProject(aFilePath, new ProjectHook(), progress, false);
    }

    if (result) {
        // The recovery file is a crash artifact: keep the project unsaved so
        // Save prompts for a real location instead of writing into the cache
        // (the tab shows "New Project" and Save As asks for a path). The
        // images themselves are embedded in the file, so the copy reopens
        // fully; only the file-watch/re-import path metadata degrades.
        result.project->setFileName("");
        resetProjectRefs(result.project);
        mProjectTabBar->pushProject(*result.project);

        mMainDisplay->resetCamera();
        mDriverHolder->driver()->updateFrame();
        mDriverHolder->driver()->updateProjectAttribute();

        // Recovered: the copy has served its purpose.
        mSystem.removeRecoveryCopy(aFilePath);
    } else {
        QMessageBox::warning(nullptr, tr("Loading Error"), result.messages());

        if (mProjectTabBar->currentProject()) {
            resetProjectRefs(mProjectTabBar->currentProject());
        }
        // Keep the recovery file: opening failed, the user may retry.
    }
}

bool MainWindow::processProjectSaving(core::Project& aProject, bool aRename, bool aSilent, bool aStopPlayback) {
    // stop animation and main display rendering
    EventSuspender suspender(*mMainDisplay, *mTarget, aStopPlayback);

    if (aProject.isNameless() || aRename) {
        QString fileName = QFileDialog::getSaveFileName(this, tr("Save File"), "", "ProjectFile (*.anie)");

        // check cancel
        if (fileName.isEmpty()) {
            return false;
        }

        if (QFileInfo(fileName).suffix().isEmpty()) {
            fileName += ".anie";
        }
        aProject.setFileName(fileName);
    }

    // save
    auto result = mSystem.saveProject(aProject, !aSilent);
    if (!result) {
        if (aSilent) {
            // Autosave must never block the user with a modal dialog.
            mViaPoint.pushLog("Failed to save project: " + result.message, ctrl::UILogType_Warn);
        } else {
            QMessageBox::warning(nullptr, "Saving Error", result.message);
        }
        return false; // failed
    }

    mProjectTabBar->updateTabNames();
    // The project now has (or keeps) a real file — its recovery copy is stale.
    mSystem.clearRecoveryCopy(aProject);
    return true;
}

void MainWindow::onSaveProjectTriggered() {
    if (mCurrent) {
        processProjectSaving(*mCurrent);
    }
}

void MainWindow::onSaveProjectAsTriggered() {
    if (mCurrent) {
        processProjectSaving(*mCurrent, true);
    }
}

void MainWindow::onCloseProjectTriggered() {
    if (mCurrent) {
        closeProjects(QVector<core::Project*>(1, mCurrent));
    }
}

void MainWindow::onTabSaveRequested(core::Project& aProject) {
    processProjectSaving(aProject);
}

void MainWindow::onTabSaveAsRequested(core::Project& aProject) {
    processProjectSaving(aProject, true);
}

void MainWindow::onTabCloseRequested(core::Project& aProject) {
    closeProjects(QVector<core::Project*>(1, &aProject));
}

void MainWindow::onTabCloseOthersRequested(core::Project& aProject) {
    QVector<core::Project*> others;
    for (int i = 0; i < mSystem.projectCount(); ++i) {
        auto* project = mSystem.project(i);
        if (project && project != &aProject) {
            others.push_back(project);
        }
    }
    closeProjects(others);
}

void MainWindow::onTabCloseAllRequested() {
    QVector<core::Project*> projects;
    for (int i = 0; i < mSystem.projectCount(); ++i) {
        auto* project = mSystem.project(i);
        if (project) {
            projects.push_back(project);
        }
    }
    closeProjects(projects);
}

void MainWindow::onTabCopyPathRequested(core::Project& aProject) {
    const QString path = aProject.fileName();
    if (!path.isEmpty()) {
        QGuiApplication::clipboard()->setText(QFileInfo(path).absoluteFilePath());
    }
}

// Variable is set both locally and in the settings, a bit overkill...
void updateSettings(QVariant *var, QSettings *settings, int value, const QString& key){
    if(var->toInt() != value){
        var->setValue(value);
        settings->setValue(key, value);
    }
}
void updateSettings(QVariant *var, QSettings *settings, bool value, const QString& key){
    if(var->toBool() != value){
        var->setValue(value);
        settings->setValue(key, value);
    }
}
void updateSettings(QVariant *var, QSettings *settings, const QString& value, const QString& key){
    if(var->toString() != value){
        var->setValue(value);
        settings->setValue(key, value);
    }
}
void updateSettings(QVariant *var, QSettings *settings, const QStringList& value, const QString& key){
    if(QStringList(var->toStringList()) != value){
        var->setValue(value);
        settings->setValue(key, value);
    }
}

// Convert the app's audio config vector into media::AudioMixer tracks.
static QVector<media::AudioMixer::Track> toAudioTracks(const std::vector<audioConfig>& aStreams) {
    QVector<media::AudioMixer::Track> tracks;
    tracks.reserve(int(aStreams.size()));
    for (const auto& ac : aStreams) {
        media::AudioMixer::Track t;
        t.path = ac.audioPath.absoluteFilePath();
        t.startFrame = ac.startFrame;
        t.endFrame = ac.endFrame;
        t.volume = AudioPlaybackWidget::getVol(ac.volume);
        t.enabled = true;
        tracks.push_back(t);
    }
    return tracks;
}

// Map the dialog's per-container encoder combo to the raw friendly encoder name
// (e.g. "x264"), or "" for Auto so the exporter picks a muxer-aware default.
static QString videoCodecForContainer(const exportParam& exParam, const QString& aContainer) {
    const defaultEncoders& e = exParam.videoParams.encoders;
    QString friendly;
    if (aContainer == QLatin1String("avi"))       friendly = aviEnc[(int)e.avi];
    else if (aContainer == QLatin1String("mkv"))       friendly = mkvEnc[(int)e.mkv];
    else if (aContainer == QLatin1String("mov"))       friendly = movEnc[(int)e.mov];
    else if (aContainer == QLatin1String("mp4"))       friendly = mp4Enc[(int)e.mp4];
    else if (aContainer == QLatin1String("webm"))      friendly = webmEnc[(int)e.webm];
    if (friendly.isEmpty() || friendly == QLatin1String("Auto") || friendly == QLatin1String("auto"))
        return QString();
    return friendly;
}

// Fill the video-specific bits of a ctrl::Exporter::VideoParam from an exportParam.
// Container can be any export target (embedded preset or a container-default codec).
static void translateVideoExport(exportParam& exParam, const std::vector<audioConfig>& aValidStreams,
                                 ctrl::Exporter::VideoParam& aVideo,
                                 QVector<media::AudioMixer::Track>& aAudio) {
    auto& g = exParam.generalParams;
    const QString container = getFormatAsString(exportTarget::video, (int)exParam.videoParams.format);
    ctrl::VideoFormat fmt = ctrl::VideoFormat::find(container);
    if (fmt.name.isEmpty()) {
        // No embedded preset (e.g. "mkv"): build a minimal format whose default
        // codec the exporter resolves via a muxer-aware container default.
        fmt.name = container;
        fmt.label = container;
    }
    aVideo.format = fmt;
    aVideo.codecIndex = -1;              // container default unless overridden below
    aVideo.codec = videoCodecForContainer(exParam, container); // friendly name or "" (Auto)
    aVideo.transparent = g.allowTransparency;
    aVideo.bps = g.bitrate;              // kbps (the exporter converts to bits/sec)
    const QString px = getFormatAsString(exportTarget::pxFmt, (int)exParam.videoParams.pixelFormat);
    aVideo.pixfmt = (px == QLatin1String("Auto") || px.isEmpty()) ? QString() : px;
    aAudio = g.exportWithAudio ? toAudioTracks(aValidStreams) : QVector<media::AudioMixer::Track>();
}

void exportProject(exportParam& exParam, core::Project* mCurrent, QDialog* widget, const std::vector<audioConfig>& validAudioStreams){
    // All full-export dialog exports now run through the single in-process
    // ctrl::Exporter (linked libav*): video, image sequence, and GIF. The legacy
    // projectExporter::Exporter + ffmpeg:: CLI machinery is no longer used.
    auto& g = exParam.generalParams;
    const bool isImage = exParam.exportType == exportTarget::image;

    // Common parameters (path differs per target below).
    ctrl::Exporter::CommonParam cparam;
    cparam.size = QSize(g.exportWidth, g.exportHeight);
    cparam.fps = g.fps > 0 ? g.fps : (g.nativeFPS > 0 ? g.nativeFPS : 24);
    // Every range the user selected, in the order they are listed. This used to
    // collapse them to [first of the first .. last of the last], so a gap was
    // exported as if it had been selected: ranges 0-10 + 50-60 wrote 61 frames
    // instead of 22. The exporter now takes the list.
    if (!g.exportRange.isEmpty()) {
        ctrl::FrameSelection selection;
        for (const frameExportRange& r : g.exportRange) {
            const int a = std::min(r.firstFrame, r.lastFrame);
            const int b = std::max(r.firstFrame, r.lastFrame);
            selection.ranges.append(ctrl::FrameRange{a, b});
        }
        cparam.selection = selection;
    } else {
        cparam.selection = ctrl::FrameSelection::fromRange(g.nativeFrameRange.firstFrame,
                                                          g.nativeFrameRange.lastFrame);
    }

    // A chosen destination is AUTHORITATIVE: it decides the container, the codec,
    // that codec's private options and stream tag, transparency, the exact output
    // size (art fitted and padded, never stretched), the frame caps, the audio
    // parameters and the frame selection. Resolved before the branches because it
    // also decides WHICH branch runs (a GIF destination is not a VideoEncoder
    // target). This is what replaces the per-container codec combos, which could
    // not work at all: getFormatAsInt() returns 0 for every input, so every codec
    // and pixel format chosen in the dialog was silently discarded.
    ctrl::ExportPlan plan;
    bool havePlan = false;
    if (!exParam.destinationId.isEmpty() && !isImage) {
        const ctrl::ExportTarget target =
            ctrl::ExportTargets::find(ctrl::ExportTargets::load(), exParam.destinationId);
        if (target.id.isEmpty()) {
            QMessageBox::warning(widget, QObject::tr("Export Error"),
                                 QObject::tr("Unknown export destination: %1").arg(exParam.destinationId));
            return;
        }
        ctrl::ExportRequest request;
        request.outputPath = g.osExportTarget;
        request.sourceSize = mCurrent->attribute().imageSize();
        request.fps = cparam.fps;
        request.selection = cparam.selection;
        // Audio is derived, not requested: how many tracks are actually meant for
        // the file, and whether the user asked for silence.
        int exportableTracks = 0;
        for (const audioConfig& ac : validAudioStreams) {
            if (ac.exportEnable) ++exportableTracks;
        }
        request.exportableAudioTracks = exportableTracks;
        request.mute = !g.exportWithAudio;

        plan = ctrl::planExport(target, request, exParam.qualityBucket);
        if (!plan.ok()) {
            QMessageBox::warning(widget, QObject::tr("Export Error"), plan.error);
            return;
        }
        havePlan = true;
        cparam.size = plan.size;
        cparam.renderSize = plan.renderSize;
        cparam.fit = plan.config.fit;
        cparam.fps = plan.fps;
        cparam.selection = plan.selection;
    }

    const bool isGif = havePlan ? plan.gifPath
                                : (exParam.exportType == exportTarget::video &&
                                   exParam.videoParams.format == availableVideoFormats::gif);

    ctrl::Exporter exporter(*mCurrent);
    exporter.setOverwriteConfirmer([widget](const QString& aPath) -> bool {
        QMessageBox m(QMessageBox::Question, QObject::tr("File exists"),
                      QObject::tr("The file \"%1\" already exists. Overwrite?").arg(aPath),
                      QMessageBox::Ok | QMessageBox::Cancel, widget);
        return m.exec() == QMessageBox::Ok;
    });
    menu::LoggableProgressReporter progress(true, widget);
    exporter.setProgressReporter(progress);

    if (isImage) {
        ctrl::Exporter::ImageParam iparam;
        cparam.path = g.exportDirectory.absolutePath();
        iparam.name = QFileInfo(g.fileName).baseName();
        iparam.suffix = getFormatAsString(exportTarget::image, (int)exParam.imageParams.format);
        iparam.quality = g.imageExportQuality;
        auto res = exporter.execute(cparam, iparam);
        if (!res && !exporter.isCanceled())
            QMessageBox::warning(widget, QObject::tr("Export Error"), exporter.log());
        return;
    }

    if (isGif) {
        ctrl::Exporter::GifParam gparam;
        cparam.path = g.osExportTarget;
        gparam.optimizePalette = true;  // legacy no-op; the in-process encoder always builds a palette
        gparam.intermediateBps = 0;
        // Custom palette: the UI collects a PNG whose pixels ARE the palette
        // (the paletteuse convention). Feed it to the encoder; otherwise it
        // builds an adaptive palette from the rendered frames.
        gparam.transparent = g.allowTransparency;
        gparam.palettePath = (g.useCustomPalette && g.palettePath.exists())
                                 ? g.palettePath.absolutePath()
                                 : QString();
        auto res = exporter.execute(cparam, gparam);
        if (!res && !exporter.isCanceled())
            QMessageBox::warning(widget, QObject::tr("Export Error"), exporter.log());
        return;
    }

    // Video.
    cparam.path = g.osExportTarget;
    ctrl::Exporter::VideoParam vparam;
    QVector<media::AudioMixer::Track> audioTracks;
    translateVideoExport(exParam, validAudioStreams, vparam, audioTracks);

    if (havePlan) {
        ctrl::VideoFormat fmt = ctrl::VideoFormat::find(plan.config.container);
        if (fmt.name.isEmpty()) {
            fmt.name = plan.config.container;
            fmt.label = plan.config.container;
        }
        vparam.format = fmt;
        vparam.codecIndex = -1;            // the codec is named explicitly
        vparam.codec = plan.config.codec;
        vparam.pixfmt = plan.config.pixFmt;
        vparam.transparent = plan.alphaEnabled;
        vparam.encOptions = plan.config.encOptions;
        vparam.codecTag = plan.config.codecTag;
        vparam.bps = 0;                    // quality comes from the codec's own controls
        vparam.audioCodec = plan.config.audioCodec;
        vparam.audioBitrate = plan.config.audioBitrate;
        vparam.audioChannels = plan.config.audioChannels;
        vparam.audioSampleRate = plan.config.audioSampleRate;
        if (!plan.audioEnabled) audioTracks.clear();
    }
    exporter.setAudioTracks(audioTracks);
    auto res = exporter.execute(cparam, vparam);
    if (!res && !exporter.isCanceled())
        QMessageBox::warning(widget, QObject::tr("Export Error"), exporter.log());
}


void MainWindow::onExportTriggered() {
    if (!mCurrent) { return; }
    if (exporting) { exportWidget->showNormal(); return; }
    // Stop animation and main display rendering
    EventSuspender suspender(*mMainDisplay, *mTarget);
    QGuiApplication::setOverrideCursor(QCursor(Qt::BusyCursor));
    QSettings settings;
    // Initialize export diag
    exporting = true;
    // Avoid weird bugs
    exportWidget = new QDialog;
    exportUI = new ExportWidgetUI;
    // Set up UI
    exportWidget->setParent(this, Qt::Window);
    exportUI->setupUi(exportWidget, mGUIResources, mCurrent->attribute().frameRange().max());

    // Destinations come from the data table — the same rows the exporter plans
    // from — so what the dialog STATES is what the export will actually do. The
    // per-container codec combos could not do that (getFormatAsInt() returns 0 for
    // every input, so every codec and pixel format chosen there was discarded).
    {
        const QList<ctrl::ExportTarget> targets = ctrl::ExportTargets::load();
        exportUI->destinationCombo->addItem(tr("(use the settings below)"), QString());
        for (const ctrl::ExportTarget& t : targets) {
            exportUI->destinationCombo->addItem(t.label, t.id);
        }
        QSettings settings;
        const int saved = exportUI->destinationCombo->findData(
            settings.value("export_destination", QString()));
        exportUI->destinationCombo->setCurrentIndex(saved > 0 ? saved : 0);
        const int savedQuality = settings.value("export_quality_bucket", 1).toInt();
        exportUI->qualityCombo->setCurrentIndex(savedQuality >= 0 && savedQuality <= 3 ? savedQuality : 1);

        auto currentBucket = [this]() {
            switch (exportUI->qualityCombo->currentIndex()) {
            case 0: return ctrl::QualityBucket::Smaller;
            case 2: return ctrl::QualityBucket::Best;
            case 3: return ctrl::QualityBucket::Lossless;
            default: return ctrl::QualityBucket::Balanced;
            }
        };

        // The plan is computed on every change and shown in words: which codec was
        // chosen and why, what size the destination forces, and every capability it
        // declares. This is the whole point of the redesign — the user states a
        // fact they possess ("it is for Telegram") and the system applies the
        // consequences.
        auto refreshFacts = [this, currentBucket]() {
            const QString id = exportUI->destinationCombo->currentData().toString();
            if (id.isEmpty()) {
                exportUI->destinationFacts->clear();
                return;
            }
            const ctrl::ExportTarget target =
                ctrl::ExportTargets::find(ctrl::ExportTargets::load(), id);

            const int first = exportUI->firstFrameSpinBox->value();
            const int last = exportUI->lastFrameSpinBox->value();
            int exportableTracks = 0;
            if (mCurrent->pConf) {
                for (const audioConfig& ac : *mCurrent->pConf) {
                    if (ac.exportEnable) ++exportableTracks;
                }
            }

            ctrl::ExportRequest request;
            request.sourceSize = mCurrent->attribute().imageSize();
            request.fps = exportUI->fpsSpinBox->value();
            request.selection = ctrl::FrameSelection::fromRange(std::min(first, last), std::max(first, last));
            request.exportableAudioTracks = exportableTracks;
            request.mute = !exportUI->exportWithAudioCheckBox->isChecked();

            const ctrl::ExportPlan plan = ctrl::planExport(target, request, currentBucket());
            QStringList lines;
            if (!plan.ok()) {
                lines << tr("This destination cannot be exported: %1").arg(plan.error);
            } else {
                lines << tr("%1 x %2, %3 fps — %4 in %5")
                             .arg(plan.size.width())
                             .arg(plan.size.height())
                             .arg(plan.fps)
                             .arg(plan.config.codec, plan.config.container);
                lines << tr("Transparency: %1   Audio: %2")
                             .arg(plan.alphaEnabled ? tr("yes") : tr("no"),
                                  plan.audioEnabled ? tr("yes") : tr("no"));
                for (const QString& note : plan.notes) {
                    lines << QStringLiteral("• ") + note;
                }
            }
            exportUI->destinationFacts->setText(lines.join(QLatin1Char('\n')));
        };

        QObject::connect(exportUI->destinationCombo,
                         util::SelectArgs<int>::from(&QComboBox::currentIndexChanged),
                         [refreshFacts](int) { refreshFacts(); });
        QObject::connect(exportUI->qualityCombo,
                         util::SelectArgs<int>::from(&QComboBox::currentIndexChanged),
                         [refreshFacts](int) { refreshFacts(); });
        refreshFacts();
    }
    // Initialize gpDiag
    gpDiag->setAttribute(Qt::WA_DeleteOnClose,true);
    gpDiag->setParent(exportWidget, Qt::Window);
    // Initialize export and general parameters
    auto* exParam = new exportParam();
    GeneralParams genParam;
    genParam.nativeWidth = mCurrent->attribute().imageSize().width();
    genParam.nativeHeight = mCurrent->attribute().imageSize().height();
    genParam.nativeFPS = mCurrent->attribute().fps();
    // I hate you gcc
    frameExportRange fer;
    fer.firstFrame = mCurrent->attribute().frameRange().min();
    fer.lastFrame = mCurrent->attribute().frameRange().max();
    genParam.nativeFrameRange = fer;
    // Get generics
    QVariant aspectRatioV = settings.value("export_aspect_ratio");
    if(!aspectRatioV.isValid()) { aspectRatioV.setValue(1); } // Aspect ratio = keep
    QVariant allowTransparencyV = settings.value("export_allow_transparency");
    if(!allowTransparencyV.isValid()) {allowTransparencyV.setValue(true); } // Allow transparency = true
    QVariant useCustomPaletteV = settings.value("export_custom_palette");
    QVariant loopAnimV = settings.value("export_loop_anim");
    QVariant exportWithAudioV = settings.value("export_allow_audio");
    // Initialize pointers to generics
    auto* aspectRatio = new int;
    auto allowTransparency = new bool;
    auto useCustomPalette = new bool;
    auto loopAnim = new bool;
    auto exportWithAudio = new bool;
    // Gather initialized pointers into a vector
    QVector<QVariant *> intValues {
        &aspectRatioV
    };
    QVector<int *> intVariables {
        aspectRatio
    };
    QVector<QVariant *> boolValues {
        &allowTransparencyV, &useCustomPaletteV,
        &loopAnimV, &exportWithAudioV
    };
    QVector<bool *> boolVariables {
        allowTransparency, useCustomPalette, loopAnim,
        exportWithAudio
    };
    // Loop through the vectors and initialize their values
    int x = 0;
    for(auto variant : intValues){
        if(!variant->isValid()){
            variant->setValue(0);
            settings.sync();
        }
        *intVariables[x] = variant->toInt();
        x++;
    }
    x= 0;
    for(auto variant : boolValues){
        if(!variant->isValid()){
            variant->setValue(false);
            settings.sync();
        }
        *boolVariables[x] = variant->toBool();
        x++;
    }
    // The per-container codec and pixel-format settings are gone with the Format
    // tab: a destination decides the codec now. The old values were never applied
    // in the first place — getFormatAsInt() returns 0 for every input, so every
    // choice they stored was read back as "Auto".
    // Get custom parameters
    // Sync
    settings.sync();
    // Value initialization
    switch(*aspectRatio){
        case 0: exportUI->oneToOneRatio->setChecked(true); break;
        case 1: exportUI->keepAspectRatio->setChecked(true); break;
        case 2: exportUI->customRatio->setChecked(true); break;
        default: break;
    }
    exportUI->transparencyCheckBox->setChecked(*allowTransparency);
    exportUI->loopCheckBox->setChecked(*loopAnim);
    exportUI->exportWithAudioCheckBox->setChecked(*exportWithAudio);
    exportUI->customPaletteCheckBox->setChecked(*useCustomPalette);
    exportUI->nativeH = genParam.nativeHeight;
    exportUI->nativeW = genParam.nativeWidth;
    exportUI->latestHeight = exportUI->nativeH;
    exportUI->latestWidth = exportUI->nativeW;
    // Project specific values
    QSignalBlocker wSB(exportUI->widthSpinBox);
    QSignalBlocker hSB(exportUI->heightSpinBox);
    wSB.reblock(); hSB.reblock();
    exportUI->widthSpinBox->setValue(genParam.nativeWidth);
    exportUI->heightSpinBox->setValue(genParam.nativeHeight);
    wSB.unblock(); hSB.unblock();
    exportUI->fpsSpinBox->setValue(genParam.nativeFPS);
    exportUI->lastFrameSpinBox->setValue(mCurrent->currentTimeInfo().frameRange.max());
    // The suggested export range starts at the project range's first frame
    // (the last frame box was set above); frames before it are pre-roll the
    // animation doesn't own.
    exportUI->firstFrameSpinBox->setValue(mCurrent->currentTimeInfo().frameRange.min());
    // Connections, these below are to force a refresh and to avoid any weird bugs or memory leaks
    connect(exportWidget, &QDialog::destroyed, [=](){
        regenerateWidget();
    });
    connect(exportWidget, &QDialog::finished, [=](){
        exportWidget->deleteLater();
    });
    connect(gpDiag, &QDialog::destroyed, [=](){
        regenerateGeneralPurposeDiag();
    });
    // ---------------------------------------------------------------------- //
    connect(exportUI->cancelButton, &QPushButton::clicked, [=]() mutable{
        exportUI->operationCancelled = true; exportWidget->close();
    });
    connect(exportUI->exportButton, &QPushButton::clicked, [=]() mutable{
        exportUI->operationCancelled = false; exportWidget->close();
    });
    connect(exportUI->setWidthNative, &QPushButton::clicked, [=](){
        exportUI->widthSpinBox->setValue(genParam.nativeWidth);
    });
    connect(exportUI->setHeightNative, &QPushButton::clicked, [=](){
        exportUI->heightSpinBox->setValue(genParam.nativeHeight);
    });
    connect(exportUI->setFPSNative, &QPushButton::clicked, [=](){
        exportUI->fpsSpinBox->setValue(genParam.nativeFPS);
    });

    // Execute
    QGuiApplication::restoreOverrideCursor();
    exportWidget->exec();

    // Save all settings
    int aspectRatioIndex = 0;
    if (exportUI->oneToOneRatio->isChecked()) { aspectRatioIndex = 0; }
    else if (exportUI->keepAspectRatio->isChecked()) { aspectRatioIndex = 1; }
    else { aspectRatioIndex = 2; }
    {
        updateSettings(&aspectRatioV, &settings, aspectRatioIndex,
                       "export_aspect_ratio");
        updateSettings(&allowTransparencyV, &settings, exportUI->transparencyCheckBox->isChecked(),
                       "export_allow_transparency");
        updateSettings(&useCustomPaletteV, &settings, exportUI->customPaletteCheckBox->isChecked(),
                       "export_custom_palette");
        updateSettings(&loopAnimV, &settings, exportUI->loopCheckBox->isChecked(),
                       "export_loop_anim");
        updateSettings(&exportWithAudioV, &settings, exportUI->exportWithAudioCheckBox->isChecked(),
                       "export_allow_audio");
        settings.sync();
    }
    // Get file name and folder for export
    if (exportUI->operationCancelled) { return; }
    // Initialize the file dialog with the appropriate descriptors
    QFileDialog fileDiag;
    fileDiag.setAcceptMode(QFileDialog::AcceptMode::AcceptSave);
    fileDiag.setFileMode(QFileDialog::AnyFile);
    QStringList vF = videoFormats;
    QStringList videoFormatDescriptor{
        tr("Animated PNG"), tr("AVI"), tr("Flash Video"), tr("Flash Video"),
        tr("GIF"), tr("Matroska"), tr("QuickTime Movie"), tr("MPEG-2"),
        tr("MPEG-4"), tr("Ogg Video"), tr("Shockwave Flash"), tr("WEBM"), tr("WEBP")
    };
    QStringList iF = imageFormats;
    QStringList imageFormatDescriptor{
        tr("BitMap"), tr("JPEG"), tr("JPEG"), tr("PNG"), tr("Portable PixelMap"),
        tr("X11 BitMap"), tr("X11 PixelMap"), tr("Tagged Image"), tr("WEBP")
    };
    x = 0;
    for(auto &format: vF){
        format.push_front(videoFormatDescriptor[x] + " (*.");
        format.push_back(')');
        x++;
    }
    x = 0;
    for(auto &format: iF){
        format.push_front(imageFormatDescriptor[x] + " (*.");
        format.push_back(')');
        x++;
    }
    if(exportUI->exportTypeCombo->currentIndex() == 0) {
        fileDiag.setNameFilters(vF);
        fileDiag.selectNameFilter("MPEG-4");
    }
    else {
        fileDiag.setNameFilters(iF);
        fileDiag.selectNameFilter("PNG");
    }
    // Generate parameters prior to exportUI destructor
    genParam.fileName = mCurrent->fileName().isEmpty()? "New Export" : QString(mCurrent->fileName());
    genParam.nativeWidth = mCurrent->attribute().imageSize().width();
    genParam.nativeHeight = mCurrent->attribute().imageSize().height();
    genParam.exportWidth = exportUI->widthSpinBox->value();
    genParam.exportHeight = exportUI->heightSpinBox->value();
    switch(aspectRatioIndex) {
    case 0:
        genParam.aspectRatio = oneToOne;
        break;
    case 1:
        genParam.aspectRatio = keep;
        break;
    case 2:
        genParam.aspectRatio = custom;
        break;
    }
    genParam.nativeFPS = mCurrent->attribute().fps();
    genParam.fps = exportUI->fpsSpinBox->value();
    genParam.bitrate =
        // Is it empty?
        exportUI->bitrateLineEdit->text().trimmed().isEmpty()
        ? 0 :
        // Does it contain only the word auto?
        exportUI->bitrateLineEdit->text().trimmed().contains(QRegularExpression("^(?i)(auto(matic)?)$"))
        ? 0 :
        // Does it contain only a positive number? If not then set to -1 for error handling.
        exportUI->bitrateLineEdit->text().trimmed().contains(QRegularExpression("^(?!0\\d+)\\d+$"))
            ? exportUI->bitrateLineEdit->text().toInt() : -1;
    genParam.imageExportQuality = exportUI->imageQualitySpinbox->value();
    genParam.allowTransparency = exportUI->transparencyCheckBox->isChecked();
    genParam.loop = exportUI->loopCheckBox->isChecked();
    genParam.exportWithAudio = exportUI->exportWithAudioCheckBox->isChecked();
    genParam.useCustomPalette = exportUI->customPaletteCheckBox->isChecked();
    genParam.palettePath = exportUI->paletteDir;
    auto* exportRanges = new QVector<frameExportRange>;
    for(int ferIndex = 0; ferIndex < exportUI->initialFrames->size(); ferIndex++){
        frameExportRange frame_export_range;
        frame_export_range.firstFrame = exportUI->initialFrames->at(ferIndex)->value();
        frame_export_range.lastFrame = exportUI->lastFrames->at(ferIndex)->value();
        exportRanges->append(frame_export_range);
    }
    genParam.exportRange = *exportRanges;
    exParam->destinationId = exportUI->destinationCombo->currentData().toString();
    switch (exportUI->qualityCombo->currentIndex()) {
    case 0: exParam->qualityBucket = ctrl::QualityBucket::Smaller; break;
    case 2: exParam->qualityBucket = ctrl::QualityBucket::Best; break;
    case 3: exParam->qualityBucket = ctrl::QualityBucket::Lossless; break;
    default: exParam->qualityBucket = ctrl::QualityBucket::Balanced; break;
    }
    {
        QSettings settings;
        settings.setValue("export_destination", exParam->destinationId);
        settings.setValue("export_quality_bucket", exportUI->qualityCombo->currentIndex());
    }
    if(exportUI->exportTypeCombo->currentIndex() == 0){ exParam->exportType = exportTarget::video; }
    else{ exParam->exportType = exportTarget::image; }
    if(exParam->exportType == exportTarget::video){
        // No pixel format or per-container encoder is collected any more: a
        // destination decides both. They stay at their defaults, which is exactly
        // what they resolved to before, because getFormatAsInt() returned 0 for
        // every input the user could pick.
    }
    // Get file via OS diag
    fileDiag.setViewMode(QFileDialog::Detail);
    QString selectedFile;
    if(fileDiag.exec()){ selectedFile = fileDiag.selectedFiles().at(0); }
    else{ return; }
    bool isVideoExport = exParam->exportType == exportTarget::video;
    int suffixIndex = isVideoExport? (int)vF.indexOf(fileDiag.selectedNameFilter()) : (int)iF.indexOf(fileDiag.selectedNameFilter());
    QString sfx = getFormatAsString(exParam->exportType, suffixIndex);
    qDebug() << "Exporting animation to format: " << sfx;
    if(QFileInfo(selectedFile).suffix() == ""){
        auto regex = QRegularExpression("(\\*.)\\w+", QRegularExpression::CaseInsensitiveOption);
        QString suffix = regex.match(fileDiag.selectedNameFilter()).captured(0).remove(0, 1);
        qDebug() << suffix;
        selectedFile.append(suffix);
    }
    qDebug() << selectedFile;
    // Generate export parameters
    genParam.exportName = QFileInfo(selectedFile).fileName();
    genParam.exportDirectory = QFileInfo(selectedFile).absoluteDir();
    genParam.exportFileName = QDir(selectedFile);
    genParam.osExportTarget = selectedFile;
    exParam->generalParams = genParam;

    if(exParam->exportType == exportTarget::video) {
        exParam->videoParams.format = static_cast<availableVideoFormats>(suffixIndex);
    }
    else{
        exParam->imageParams.format = static_cast<availableImageFormats>(suffixIndex);
    }
    // Main export function
    std::vector<audioConfig> validAudioStreams = AudioPlaybackWidget::getValidAudioStreams(*mCurrent->pConf);
    if(isExportParamValid(exParam, exportWidget, &validAudioStreams)){
        exportProject(*exParam, mCurrent, exportWidget, validAudioStreams);
    }
}

void MainWindow::onExportImageSeqTriggered(const QString& aSuffix) {
    if (!mCurrent)
        return;

    // stop animation and main display rendering
    EventSuspender suspender(*mMainDisplay, *mTarget);

    // export directory
    QString dirName = QFileDialog::getExistingDirectory(this, tr("Export Folder"));

    // make sure existing
    if (dirName.isEmpty())
        return;
    if (!QFileInfo::exists(dirName))
        return;

    // export param
    ctrl::Exporter::CommonParam cparam;
    ctrl::Exporter::ImageParam iparam;
    {
        QScopedPointer<ImageExportDialog> dialog(new ImageExportDialog(*mCurrent, dirName, aSuffix, this));

        dialog->exec();
        if (dialog->result() != QDialog::Accepted)
            return;

        cparam = dialog->commonParam();
        iparam = dialog->imageParam();
    }

    // gui for confirm overwrite
    auto overwriteConfirmer = [=](const QString&) -> bool {
        QMessageBox msgBox;
        msgBox.setText(tr("File already exists."));
        msgBox.setInformativeText(tr("Do you want to overwrite the existing file?"));
        msgBox.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
        msgBox.setDefaultButton(QMessageBox::Ok);
        int ret = msgBox.exec();
        return (ret == QMessageBox::Ok);
    };

    menu::ProgressReporter progress(true, this);
    ctrl::Exporter exporter(*mCurrent);
    exporter.setOverwriteConfirmer(overwriteConfirmer);
    exporter.setProgressReporter(progress);

    // execute
    if (!exporter.execute(cparam, iparam)) {
        progress.cancel();
        if (!exporter.isCanceled()) {
            QMessageBox::warning(nullptr, tr("Export Error"), exporter.log());
        }
    }
}

void MainWindow::onQuickExportTriggered(const QString& aFormat) {
    // General parameters
    GeneralParams genParam;
    genParam.fileName = mCurrent->fileName();
    if (genParam.fileName.isEmpty()) {
        genParam.fileName = "New Export";
    }
    genParam.exportName = QFileInfo(genParam.fileName).baseName();
    genParam.exportDirectory = QFileDialog::getExistingDirectory(this, tr("Export Folder"));
    genParam.exportFileName = QDir(genParam.exportDirectory.absolutePath() + "/" + genParam.exportName + "." +
        QString(aFormat).replace("_o", "").replace("_t", "").replace(".anie", ""));
    genParam.osExportTarget = QFileInfo(genParam.exportName).absoluteFilePath();
    genParam.nativeWidth = mCurrent->attribute().imageSize().width();
    genParam.nativeHeight = mCurrent->attribute().imageSize().height();
    genParam.exportWidth = mCurrent->attribute().imageSize().width();
    genParam.exportHeight = mCurrent->attribute().imageSize().height();
    genParam.aspectRatio = custom;
    genParam.nativeFPS = mCurrent->attribute().fps();
    genParam.fps = mCurrent->attribute().fps();
    genParam.bitrate = 0;
    genParam.imageExportQuality = 100;
    genParam.allowTransparency = aFormat.contains("_t") || aFormat == "png";
    genParam.loop = true;
    genParam.exportWithAudio = !mCurrent->pConf->empty();
    frameExportRange fer;
    fer.firstFrame = mCurrent->attribute().frameRange().min();
    fer.lastFrame = mCurrent->attribute().frameRange().max();
    genParam.nativeFrameRange = fer;
    genParam.exportAllFrames = true;
    genParam.exportRange = {fer};
    genParam.exportToLast = true;
    genParam.imageExportQuality = -1;
    // Get format info
    bool imageExport = false;
    ImageParams img;
    VideoParams vid;
    if (aFormat == "png") {
        imageExport = true;
        img.format = availableImageFormats::png;
    }
    else {
        if (aFormat.contains("gif")) {
            vid.format = availableVideoFormats::gif;
        }
        else if (aFormat.contains("mp4")) {
            vid.format = availableVideoFormats::mp4;
        }
        else if (aFormat.contains("avi")) {
            vid.format = availableVideoFormats::avi;
        }
        else if (aFormat.contains("webm")) {
            vid.format = availableVideoFormats::webm;
        }
    }
    // Export parameters
    auto exParam = new exportParam();
    exParam->generalParams = genParam;
    exParam->imageParams = img;
    exParam->videoParams = vid;
    exParam->exportType = imageExport? exportTarget::image : exportTarget::video;

    // Main export function
    std::vector<audioConfig> validAudioStreams = AudioPlaybackWidget::getValidAudioStreams(*mCurrent->pConf);
    if(isExportParamValid(exParam, exportWidget, &validAudioStreams)){
        exportProject(*exParam, mCurrent, exportWidget, validAudioStreams);
    }
}

void MainWindow::onExportVideoTriggered(const ctrl::VideoFormat& aFormat) {
    if (!mCurrent)
        return;

    const QString suffix = aFormat.name;
    const bool isGif = suffix == "gif";

    // stop animation and main display rendering
    EventSuspender suspender(*mMainDisplay, *mTarget);

    const QString targetVideos = "Videos (*." + suffix + ")";

    // get export file name
    QString fileName = QFileDialog::getSaveFileName(
        this,
        tr("Export File"),
        QString(), // dir
        targetVideos
    );
    const QFileInfo fileInfo(fileName);

    // make sure existing
    if (fileName.isEmpty())
        return;
    if (!fileInfo.dir().exists())
        return;

    if (fileInfo.suffix().isEmpty()) {
        fileName += "." + suffix; // makesure suffix
    } else if (fileInfo.suffix() != suffix) {
        QMessageBox::warning(nullptr, tr("Operation Error"), tr("Invalid extension specified."));
        return;
    }

    // export param
    ctrl::Exporter::CommonParam cparam;
    ctrl::Exporter::VideoParam vparam;
    ctrl::Exporter::GifParam gparam;
    if (isGif) {
        QScopedPointer<GifExportDialog> dialog(new GifExportDialog(*mCurrent, fileName, this));
        dialog->exec();
        if (dialog->result() != QDialog::Accepted)
            return;

        cparam = dialog->commonParam();
        gparam = dialog->gifParam();
    } else {
        QScopedPointer<VideoExportDialog> dialog(new VideoExportDialog(*mCurrent, fileName, aFormat, this));
        dialog->exec();
        if (dialog->result() != QDialog::Accepted)
            return;

        cparam = dialog->commonParam();
        vparam = dialog->videoParam();
    }
    // vparam.codec = codec;

    menu::LoggableProgressReporter progress(true, this);
    ctrl::Exporter exporter(*mCurrent);
    exporter.setOverwriteConfirmer([=](const QString&) -> bool { return true; });
    exporter.setProgressReporter(progress);
    exporter.setUILogger(progress);

    // Include the project's multi-track audio on video exports: it is mixed and
    // muxed in-process (linked libav*) with the video in one pass.
    if (!isGif) {
        const auto validAudio = AudioPlaybackWidget::getValidAudioStreams(*mCurrent->pConf);
        QVector<media::AudioMixer::Track> audioTracks;
        audioTracks.reserve(int(validAudio.size()));
        for (const auto& ac : validAudio) {
            media::AudioMixer::Track t;
            t.path = ac.audioPath.absoluteFilePath();
            t.startFrame = ac.startFrame;
            t.endFrame = ac.endFrame;
            t.volume = AudioPlaybackWidget::getVol(ac.volume);
            t.enabled = true;
            audioTracks.push_back(t);
        }
        exporter.setAudioTracks(audioTracks);
    }

    // execute
    auto result = isGif ? exporter.execute(cparam, gparam) : exporter.execute(cparam, vparam);

    if (!result) {
        progress.cancel();

        if (result.code == ctrl::Exporter::ResultCode_Canceled) {
        } else if (result.code == ctrl::Exporter::ResultCode_EncoderOpenFailed) {
            QMessageBox message;
            message.setIcon(QMessageBox::Warning);
            message.setText(tr("FFmpeg was not found."));
            auto infoText = tr("Video export requires FFmpeg.") + "\n" +
                tr("Install FFmpeg on the system, or place a FFmpeg executable "
                   "under \"/tools\" in the folder where you installed AnimeEffects.");
            message.setInformativeText(infoText);
            message.setStandardButtons(QMessageBox::Ok);
            message.setDefaultButton(QMessageBox::Ok);
            message.exec();
        } else {
            QMessageBox::warning(nullptr, tr("Export Error"), exporter.log());
        }
    }
}
void MainWindow::onMoveFrameTriggered(bool moveRight) {
    int frameDest = moveRight? 1: -1;
    core::Frame dest = mTarget->currentFrame();
    dest.add(frameDest);
    mTarget->timeLineWidget().setFrame(dest);
}

} // namespace gui
