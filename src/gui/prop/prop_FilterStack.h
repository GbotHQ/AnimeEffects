#ifndef GUI_PROP_FILTERSTACK_H
#define GUI_PROP_FILTERSTACK_H

#include <QVector>
#include "core/Project.h"
#include "core/ObjectNode.h"
#include "core/TimeKeyType.h"
#include "gui/prop/prop_KeyAccessor.h"
#include "gui/prop/prop_Panel.h"

class QFrame;
class QLabel;
class QPushButton;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace gui {
class GUIResources;
namespace prop {

class CheckItem;
class DecimalItem;
class IntegerItem;
class ItemBase;

// THE FILTER STACK.
//
// HSV and Blur used to be two independent "Current Keys" sections, each hidden
// unless the current frame happened to hold one of their keys. A layer's effects
// are a STACK: one list, one order, one eye per entry, and a detail editor for the
// selected entry. This panel is that list.
//
// The values are still the existing HSV/Blur key lanes, so animation, the graph,
// the timeline, and the undo stack keep their existing semantics. The stack itself
// stores only ORDER and ENABLED (core::TimeLine::filters()); adding a filter is a
// stack edit and the first field edit creates its key at the current frame. A
// filter lane that exists without a stack entry is appended to the effective stack,
// which is what makes a legacy project and an "Add HSV key" from the timeline show
// up here.
//
// UX rules this panel keeps:
//   * rows are a real stack: drag a row to reorder, click to edit, eye to mute,
//     trash to remove (with undo);
//   * "Add filter" opens a SEARCHABLE catalog. The search field is the point: a
//     filter list is a menu of specific things, not a browse-all tree;
//   * Wave Deform and Custom GLSL are listed in the catalog as explicit coming-soon
//     stubs. They cannot be added yet, so they never create an empty stack row;
//   * a filter that has no key at the current frame still shows its living
//     (interpolated) values. Editing adds the key, exactly like the other
//     properties, instead of refusing the edit.
class FilterStackPanel: public Panel {
    Q_OBJECT
public:
    FilterStackPanel(core::Project& aProject, QWidget* aParent, GUIResources* aGUIResources);
    ~FilterStackPanel() override;

    void setTarget(core::ObjectNode* aTarget);
    void updateKey();
    void updateFrame();

private:
    struct Row;

    struct CatalogEntry {
        QString name;
        QString description;
        core::TimeKeyType type = core::TimeKeyType_TERM;
        bool available = true;
    };

    static QVector<CatalogEntry> catalog();
    static QString displayName(core::TimeKeyType aType);

    core::TimeLine* line() const;
    int frame() const;
    bool hasKey(core::TimeKeyType aType) const;

    void rebuildRows(const QVector<core::FilterEntry>& aEntries);
    void refreshRowState();
    void refreshEditor();
    void toggleRow(Row* aRow);
    void placeDetail();
    void showFilterPicker();
    void addFilter(core::TimeKeyType aType);
    void addKey(core::TimeKeyType aType);
    void removeFilter(core::TimeKeyType aType);
    void toggleFilter(core::TimeKeyType aType, bool aEnabled);
    void moveFilter(core::TimeKeyType aType, int aIndex);

    void buildDetailEditors();

    // Drag/drop of stack rows. The row tells the panel a global y; the panel paints
    // a drop line (not a live reorder) and commits ONE move on release.
    void beginRowDrag(QWidget* aRow, int aGlobalY);
    void moveRowDrag(int aGlobalY);
    void endRowDrag();
    int dropIndexAt(int aGlobalY) const;

    QVector<Row*> mRows;
    QVector<core::FilterEntry> mEntries;

    core::Project& mProject;
    GUIResources* mResources;
    core::ObjectNode* mTarget;
    KeyAccessor mAccessor;
    core::TimeKeyType mSelected = core::TimeKeyType_TERM;

    QWidget* mBody;
    QWidget* mRowsHost;
    QVBoxLayout* mRowsLayout;
    QLabel* mEmptyLabel;
    QToolButton* mAddButton;
    QWidget* mDetailHost;
    QVBoxLayout* mDetailLayout;

    QWidget* mHsvEditor;
    IntegerItem* mHue;
    IntegerItem* mSaturation;
    IntegerItem* mValue;
    CheckItem* mAbsolute;

    QWidget* mBlurEditor;
    DecimalItem* mBlurAmount;
    CheckItem* mBlurDirectional;
    DecimalItem* mBlurX;
    DecimalItem* mBlurY;
    DecimalItem* mBlurAngle;
    QLabel* mBlurXLabel;
    QLabel* mBlurYLabel;
    QLabel* mBlurAngleLabel;

    QVector<ItemBase*> mDetailItems;

    QFrame* mDropLine;
    QWidget* mDragRow;
    int mDropIndex;
    bool mDragActive;
};

} // namespace prop
} // namespace gui

#endif // GUI_PROP_FILTERSTACK_H
