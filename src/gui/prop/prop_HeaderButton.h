#ifndef GUI_PROP_HEADERBUTTON_H
#define GUI_PROP_HEADERBUTTON_H

#include <QAbstractButton>
#include <QIcon>
#include <QPixmap>

class QLabel;
class QHBoxLayout;

namespace gui {

class GUIResources;

namespace prop {

    // Full-row section header. The whole band is the click target (the old
    // checkable-QGroupBox header accepted clicks only on the style-painted
    // title/indicator subrects, leaving the rest of the row dead).
    //
    // Composition: a fixed caret column (the glyph centered) followed by
    // the title as a plain QLabel. The column width is constant across the
    // header levels, so the caret centers land on the same axis and every
    // title starts at the same x -- independent of the QPushButton's
    // style-dependent icon/text spacing. The caret rests dim and brightens
    // on hover (enter/leave); the row tint/typography come from the QSS.
    class HeaderButton: public QAbstractButton {
        Q_OBJECT
    public:
        // aIconSize: 14 for the top-level headers, 12 for the subsections
        // (the carets track the font-size hierarchy: 9pt vs 8pt)
        HeaderButton(
            const QString& aText, GUIResources* aGUIResources,
            const QString& aObjectName, QWidget* aParent = nullptr, int aIconSize = 14
        );

        // A right-aligned action in the header row (the Filters section's plus).
        // It sits after the stretch, so the title stays left and the action right.
        void addTrailingWidget(QWidget* aWidget);
        // A control that belongs to the TITLE (the key-state dot), not to the
        // row's trailing edge. It is inserted after the title and before the
        // stretch, so a key mark always reads as part of the property's name.
        void addTitleAction(QWidget* aWidget);
        // A leading gutter control (the property key dot). It sits before the
        // title, so adding/removing it never shifts the field column.
        void addLeadingAction(QWidget* aWidget);
        void setContentInset(int aLeft);
        // A flat property row owns no collapse state; hiding the caret is the
        // visual half of that (KeyGroup also stops accepting the toggle).
        void setCaretVisible(bool aVisible);
        // Fixed title column: the key dot after it then lands on one x for
        // every property row.
        void setTitleWidth(int aWidth);
        // A field that belongs on the header line. It takes the row's spare
        // width instead of sitting after the stretch at its minimum size.
        void addInlineField(QWidget* aWidget);

    protected:
        QSize sizeHint() const override;
        // the caret and title labels render the row; the button itself
        // paints nothing (its background comes from the QSS)
        void paintEvent(QPaintEvent* aEvent) override;
        void enterEvent(QEnterEvent* aEvent) override;
        void leaveEvent(QEvent* aEvent) override;

    private:
        void updateCaret();

        static const int kCaretColumn = 20;

        QLabel* mCaret;
        QLabel* mTitle;
        QHBoxLayout* mLayout;
        bool mHovered;
        QPixmap mRightIcon;  // bright, expanded-direction carets
        QPixmap mDownIcon;
        QPixmap mRightDim;   // dim (resting) variants
        QPixmap mDownDim;
    };

} // namespace prop
} // namespace gui

#endif // GUI_PROP_HEADERBUTTON_H
