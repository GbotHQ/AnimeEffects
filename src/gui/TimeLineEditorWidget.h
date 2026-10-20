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

class QMenu;

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
    // The temporary playback window changed (a shift-drag on the ruler set, moved or
    // collapsed it, or the context menu cleared it). The window is the timeline's own
    // state, and the animation graph panel's ruler overlays it too, so the GUI needs a
    // notification when the gesture lands - the frame window's onViewRangeChanged is
    // not emitted by a window drag that moves no frame.
    Q_SIGNAL void playWindowChanged();

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
    // Opens the application's own KEY MENU at aGlobalPos, for the key aPressedKey
    // names under the caller's pointer: the menu a right-click in the timeline
    // itself builds (see buildKeyMenu), so both views offer the same items through
    // the same code and there is no second menu to keep in step.
    //
    // THE SET THE ITEMS ACT ON is the shared live selection, read here with the
    // timeline's own right-click rule (the one tests/rightclick_transient_selection.cpp
    // pins): a pressed key the selection already holds keeps the WHOLE selection, so
    // a menu opened on one key of a multi-key selection still names every selected
    // key; any other pressed key is lent a temporary selection for the menu's
    // lifetime, which is dropped when the menu closes (the previous selection is NOT
    // restored). This is the entry point for callers whose pointer is in THEIR own
    // space - the animation graph panel resolves the key with its own hit test,
    // gui::panelKeyAt - so the rule is applied here, on the shared selection, by key
    // identity: ctrl::TimeLineEditor::selectKeysAt resolves a click in the timeline's
    // own pixels and cannot be reached from another view's geometry.
    //
    // The menu's paste entry anchors at aPressedKey's frame. False when there is no
    // menu to open: no editor, or aPressedKey is not a key the document holds.
    bool openKeyMenu(const core::TimeLineEvent::Target& aPressedKey, const QPoint& aGlobalPos);
    // The easing control the header hosts (prop::EasingItem). This widget is the
    // one place the picker is opened and the one place a committed edit lands, so
    // the control itself holds no model; the header sets it once, and clears it
    // when it goes away (null is a valid, inert state). The chip's state is
    // PUSHED to it (see pushEasingChipState), not asked for: this surface is where
    // the shared key selection is re-read, and the header has no other way to learn
    // that another view changed it.
    void setEasingItemHost(prop::EasingItem* aHost);
    // Opens THAT picker for the keys aTargets names, anchored at aGlobalAnchor -
    // which is the header's chip (gui::TimeLineHeader::onEasingChipClicked, the one
    // caller): the picker is a popover of the header's easing control, and the key
    // menus carry no easing entry, so this is not reached from a key at all. The
    // targets are held for the open's lifetime, so every edit the picker commits
    // applies to exactly the keys it was opened for, until the popover closes.
    //
    // False when nothing opened (no host, no targets, or no key of a type that
    // carries an easing).
    bool openEasingPicker(const core::TimeLineEvent& aTargets, const QPoint& aGlobalAnchor);
    // THE HEADER'S QUICK-EASING BUTTONS, in the timeline's own currency: write
    // aEasing (or the Auto preset's mode pair) to every component the live
    // selection names, as ONE undo step. They live here rather than on the
    // animation graph's editor because the selection is the TIMELINE's: a Pose or
    // FFD key has one shared easing but no graph channel, and the graph's action
    // refuses it - the quick easings must not. FALSE when nothing was written
    // (no selection, only bone/mesh keys, or an edit that changes nothing).
    bool applyEasingToSelection(util::Easing::Type aType);
    bool applyAutoHandlesToSelection();
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
    // THE WINDOW GESTURE FROM THE OTHER RULER: ctrl::GraphEditor's band delegates
    // its Shift-press/drag/release here (via gui::TimeLineWidget), one step at a
    // time, on this editor's own ctrl::TimeLineEditor::PlayWindow. Emits
    // playWindowChanged on a value change - the same notification the timeline's
    // own Shift-drag produces, so the graph's overlay is re-pushed with it, and the
    // panel's copy is never a step behind the timeline's window - then repaints both
    // rulers (the surface's track and the separate ruler strip). Returns true when
    // the window changed.
    //
    // Defined here so the gesture stays one expression of the editor's own three
    // steps; it touches only state this class already owns (mEditor, mTimeRuler, the
    // signal).
    bool updatePlayWindow(ctrl::TimeLineEditor::PlayWindowStep aStep, core::Frame aFrame) {
        const bool changed = mEditor->updatePlayWindow(aStep, aFrame);
        if (changed) {
            emit playWindowChanged();
            this->update();
            if (mTimeRuler)
                mTimeRuler->update();
        }
        return changed;
    }

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
    // THE SURFACE'S KEYBOARD. The surface takes focus on a click (it is the widget
    // the pointer lands on, and it has Qt::ClickFocus: a click here hands it the
    // keyboard without adding a Tab stop beside the timeline's own, and the press
    // itself still propagates to TimeLineWidget's gestures), and it owns exactly one
    // key:
    //   * Ctrl+A - ctrl::TimeLineEditor::selectAllKeys(), every key of every row the
    //     timeline shows, then a repaint.
    // Every other key is IGNORED, so it propagates to TimeLineWidget (the arrow-key
    // panning) and from there to the application's key map, exactly as before this
    // widget could hold focus.
    virtual void keyPressEvent(QKeyEvent* aEvent);
    void onTimeLineModified(core::TimeLineEvent& aEvent, bool aUndo);
    void onTreeRestructured(core::ObjectTreeEvent& aEvent, bool aUndo);
    void onProjectAttributeModified(core::ProjectEvent& aEvent, bool aUndo);
    void onContextMenuRequested(const QPoint& aPos);
    // THE KEY MENU ITSELF, built once for both routes: the timeline's own right-click
    // (onContextMenuRequested) and the animation graph panel's (openKeyMenu). The
    // items act on mTargets - the whole selection, one target per key - so a
    // multi-key selection is one edit per item, not one key's; the paste entry is
    // enabled by what has been copied (mCopyTargets), and anchors at mPastePos,
    // which each caller sets to the frame the menu was opened on. An EMPTY mTargets
    // is the paste-only menu the timeline shows when a right-click names no key and
    // nothing is selected.
    void buildKeyMenu(QMenu& aMenu);
    void onCopyKeyTriggered(bool);
    void onCopyCBTriggered(bool);
    void onPasteKeyTriggered(bool);
    void onDeleteKeyTriggered(bool);
    void onSelectSpacingTriggered();
    // Hands the header's chip what it names: one easing per selected KEY, in
    // selection order, read from the FIRST marker the selection names for it (so a
    // Y-only selection shows Y's curve, not the key's first component - the first
    // component the selection names, the SAME answer openEasingPicker uses), plus
    // the Auto context of that component, so its glyph is the curve that component
    // plays rather than a shape no key has. A whole-key entry names every component,
    // so it reads the first. When the shown key's selection spans SEVERAL components
    // the chip is handed their names as a scope note (its tooltip), because its one
    // painted value is then one of several and the picker edits them all.
    // Keys of a type that carries no easing (bone, mesh) contribute nothing, and so
    // does a selection entry whose key the document no longer holds - the key is
    // resolved from the document, as ctrl::time::Selection's contract requires (see
    // the definition). IDEMPOTENT: prop::EasingChip ignores a state equal to the one
    // it holds, so an extra call costs one compare.
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
