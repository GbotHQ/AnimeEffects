#ifndef GUI_TIMELINEINNERWIDGET_H
#define GUI_TIMELINEINNERWIDGET_H

#include <QWidget>
#include <QAction>
#include <QTreeWidgetItem>
#include <functional>
#include "core/TimeLineEvent.h"
#include "ctrl/TimeLineEditor.h"
#include "gui/ViaPoint.h"
#include "gui/GUIResources.h"
#include "gui/TimeRuler.h"
#include "KeyCommandMap.h"

namespace gui {

namespace prop { class EasingItem; } // the header's easing control (prop_Items.h)

class TimeCursor: public QWidget {
    Q_OBJECT

    Q_PROPERTY(QColor numberColor READ numberColor WRITE setNumberColor DESIGNABLE true)
    Q_PROPERTY(QColor edgeColor READ edgeColor WRITE setEdgeColor DESIGNABLE true)
public:
    TimeCursor(QWidget* aParent);
    void setCursorPos(const QPoint& aPos, int aHeight);
    void setHeaderInfo(const QString& aFrameText, const QColor& aBadgeColor);
    virtual void paintEvent(QPaintEvent* aEvent);

    QColor numberColor() const;
    void setNumberColor(const QColor& numberColor);

    QColor edgeColor() const;
    void setEdgeColor(const QColor& edgeColor);

private:
    QColor mNumberColor;
    QColor mEdgeColor;
    QString mFrameText;
    QColor mBadgeColor;
    int mLineX;
};

class TimeLineEditorWidget: public QWidget {
    Q_OBJECT

    // Properties introduced to let users theme the custom rendered parts of the widget.
    // Qt's stylesheet support for custom rendering is applied per widget via properties.
    Q_PROPERTY(QColor headerContentColor READ headerContentColor WRITE setHeaderContentColor DESIGNABLE true)
    Q_PROPERTY(QColor headerBackgroundColor READ headerBackgroundColor WRITE setHeaderBackgroundColor DESIGNABLE true)
    Q_PROPERTY(QColor rulerLineColor READ rulerLineColor WRITE setRulerLineColor DESIGNABLE true)


    Q_PROPERTY(QColor trackColor READ trackColor WRITE setTrackColor DESIGNABLE true)
    Q_PROPERTY(QColor trackEdgeColor READ trackEdgeColor WRITE setTrackEdgeColor DESIGNABLE true)
    Q_PROPERTY(QColor trackTextColor READ trackTextColor WRITE setTrackTextColor DESIGNABLE true)
    Q_PROPERTY(QColor trackSelectColor READ trackSelectColor WRITE setTrackSelectColor DESIGNABLE true)
    Q_PROPERTY(QColor trackSeperatorColor READ trackSeperatorColor WRITE setTrackSeperatorColor DESIGNABLE true)

public:
    TimeLineEditorWidget(ViaPoint& aViaPoint, QWidget* aParent);

    // Ruler gestures (seek / playback-window drag) move the current frame;
    // TimeLineWidget forwards this to its onFrameUpdated signal.
    Q_SIGNAL void rulerFrameChanged();

    void setProject(core::Project* aProject);
    void setFrame(core::Frame aFrame);

    void updateCamera(const core::CameraInfo& aCamera);
    void updateLines(QTreeWidgetItem* aTopNode);
    void updateLineSelection(core::ObjectNode* aRepresent);
    bool updateCursor(const core::AbstractCursor& aCursor, Qt::KeyboardModifiers aModifiers);
    bool updateWheel(QWheelEvent* aEvent, int aMouseX, int& aPixelAfter);
    void updateProjectAttribute();
    void updateTheme(theme::Theme&);

    // The frame range's extent on the content axis (px): the left margin
    // gutter plus the scale position of the range's start/end frames.
    int frameStartPixel() const;
    int frameEndPixel() const;

    QSize getEditorSize() const;

    core::Frame currentFrame() const;
    // The controller the surface renders; TimeRuler drives it through the
    // same updateCursor entry point as the track input path.
    ctrl::TimeLineEditor* editor() const { return mEditor.data(); }
    // Shows the application's own KEY EASING PICKER at aGlobalPos, for the keys
    // aTargets names (the caller's current selection, in the timeline's
    // currency). This is the entry point for callers that are not the timeline's
    // own context-menu handler - the animation graph panel's right-click - and it
    // is the SAME picker the header's easing chip opens (that popover is this
    // widget's single instance), so the two anchors cannot offer different
    // easings or take different edit paths. Nothing here depends on the panel: a
    // caller with a selection and a screen position gets the picker, and an EMPTY
    // target list shows nothing.
    void showKeyEasingPicker(const core::TimeLineEvent& aTargets, const QPoint& aGlobalPos);
    // The easing control the header hosts (prop::EasingItem). This widget is the
    // one place the picker is opened and the one place a committed edit lands, so
    // the control itself holds no model; the header sets it once, and clears it
    // when it goes away (null is a valid, inert state). The chip's state is
    // PUSHED to it (see pushEasingChipState), not asked for: this surface is where
    // the shared key selection is re-read, and the header has no other way to learn
    // that another view changed it.
    void setEasingItemHost(prop::EasingItem* aHost);
    // Opens THAT picker for the keys aTargets names, anchored at aGlobalAnchor:
    // under the header's chip, or at the pointer a right-click on a key landed at.
    // The
    // targets are held for the open's lifetime, so every edit the picker commits
    // applies to exactly the keys it was opened for (a right-click on an unselected
    // key lends it a temporary selection - see ctrl::TimeLineEditor::selectKeysAt -
    // and it outlives this call, until the popover closes).
    //
    // False when nothing opened (no host, no targets, or no key of a type that
    // carries an easing): the caller's temporary selection is then its own to end.
    bool openEasingPicker(const core::TimeLineEvent& aTargets, const QPoint& aGlobalAnchor);
    // The MoveCanvas key (space by default) is held: the ruler lets a
    // left-press propagate to the timeline's pan instead of seeking.
    void setPanKeyHeld(bool aHeld) {
        if (mTimeRuler)
            mTimeRuler->setPanKeyHeld(aHeld);
    }
    // The effective playback range (project range or the temporary window
    // override); TimeLineWidget's playback tick and the playhead clamp use it.
    util::Range playbackRange() const { return mEditor->playbackRange(); }
    // The temporary playback window (see ctrl::TimeLineEditor::PlayWindow);
    // the renderer draws its band and brackets.
    const ctrl::TimeLineEditor::PlayWindow& playWindow() const { return mEditor->playWindow(); }

    QColor headerContentColor() const;
    void setHeaderContentColor(const QColor& headerContentColor);

    QColor headerBackgroundColor() const;
    void setHeaderBackgroundColor(const QColor& headerBackgroundColor);

    QColor rulerLineColor() const;
    void setRulerLineColor(const QColor& rulerLineColor);


    QColor trackColor() const;
    void setTrackColor(const QColor& trackColor);

    QColor trackEdgeColor() const;
    void setTrackEdgeColor(const QColor& trackEdgeColor);

    QColor trackTextColor() const;
    void setTrackTextColor(const QColor& trackTextColor);

    QColor trackSelectColor() const;
    void setTrackSelectColor(const QColor& trackSelectColor);

    QColor trackSeperatorColor() const;
    void setTrackSeperatorColor(const QColor& trackSeperatorColor);


private:
    void updateTimeCursorPos();
    void updateSize();
    void updateLinesRecursive(QTreeWidgetItem* aItem);
    virtual void paintEvent(QPaintEvent* aEvent);
    virtual void resizeEvent(QResizeEvent* aEvent);
    void onTimeLineModified(core::TimeLineEvent& aEvent, bool aUndo);
    void onTreeRestructured(core::ObjectTreeEvent& aEvent, bool aUndo);
    void onProjectAttributeModified(core::ProjectEvent& aEvent, bool aUndo);
    void onContextMenuRequested(const QPoint& aPos);
    void onCopyKeyTriggered(bool);
    void onCopyCBTriggered(bool);
    void onPasteKeyTriggered(bool);
    void onDeleteKeyTriggered(bool);
    void onSelectSpacingTriggered();
    void onSelectRangeTriggered(int rangeType);
    // Applies aNext to the easing of every key the current open named and commits
    // the results as ONE undo step (one macro, one notification naming every key
    // written). A key whose type carries no easing is skipped, and so is one whose
    // value aNext would not change, so an edit that changes nothing leaves no
    // history entry behind. This is the picker's commit path and the range menu's.
    void assignEasingToTargets(const std::function<util::Easing::Param(const util::Easing::Param&)>& aNext);
    // Hands the header's chip what it names: one easing per SELECTED key, in
    // selection order, plus the Auto context of the key the chip shows (the first),
    // so its glyph is the curve that key plays rather than a shape no key has. Keys
    // of a type that carries no easing (bone, mesh) contribute nothing, and so does a
    // selection entry whose key the document no longer holds - the key is resolved
    // from the document, as ctrl::time::Selection's contract requires (see the
    // definition). IDEMPOTENT: prop::EasingChip ignores a state equal to the one it
    // holds, so an extra call costs one compare.
    //
    // Called from the paint (see paintEvent), when a host is attached or the picker
    // opens, on a document modification (see onTimeLineModified), and - the source
    // that closes the last gap - by the selection's own change notification
    // (ctrl::time::Selection::onChanged), which fires whichever view made the
    // selection and needs nothing to be visible or painted.
    void pushEasingChipState();

    ViaPoint& mViaPoint;
    util::LinkPointer<core::Project> mProject;
    util::SlotId mTimeLineSlot;
    util::SlotId mTreeRestructSlot;
    util::SlotId mProjectAttrSlot;
    QScopedPointer<ctrl::TimeLineEditor> mEditor;
    const core::CameraInfo* mCamera;
    TimeCursor mTimeCursor;
    // The ruler strip: a real widget with its own paint and input, layered
    // under the playhead (TimeCursor) and over the track surface.
    QScopedPointer<TimeRuler> mTimeRuler;

    //@todo Implement deserializer to put timeevent data into a bytebuffer or plain text

    KeyCommandMap& mKeyCommandMap;
    QAction* mCopyKey;
    QAction* mCopyToClipboard;
    QAction* mSetSpacing;
    QAction* mPasteKey;
    QAction* mDeleteKey;
    QAction* mSelectSpacing;
    QMenu* mSelectRange;
    // The header's easing control, if a header hosts one: it owns the single
    // picker popover this widget opens (see openEasingPicker).
    prop::EasingItem* mEasingHost;
    core::TimeLineEvent mTargets;
    core::TimeLineEvent mCopyTargets;
    QPoint mPastePos;
    bool mOnPasting;

    theme::TimeLine mTimelineTheme;
};

} // namespace gui

#endif // GUI_TIMELINEINNERWIDGET_H
