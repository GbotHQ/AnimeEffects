#ifndef GUI_QML_FILTERSTACKMODEL_H
#define GUI_QML_FILTERSTACKMODEL_H

#include <QColor>
#include <QCoreApplication>
#include <QObject>
#include <QVariantList>
#include <QVector>

#include "core/TimeKeyType.h"
#include "core/TimeLine.h"

namespace gui {

class InspectorViewModel;

class FilterRowModel: public QObject {
    Q_OBJECT
    Q_PROPERTY(int type READ type CONSTANT)
    Q_PROPERTY(QString kind READ kind CONSTANT)
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(bool expanded READ expanded NOTIFY changed)
    Q_PROPERTY(QString keyState READ keyState NOTIFY changed)
    Q_PROPERTY(QColor keyColor READ keyColor NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(QVariantList summarySegments READ summarySegments NOTIFY changed)
    Q_PROPERTY(QString hueLabel READ hueLabel CONSTANT)
    Q_PROPERTY(QString saturationLabel READ saturationLabel CONSTANT)
    Q_PROPERTY(QString valueLabel READ valueLabel CONSTANT)
    Q_PROPERTY(QString colorLabel READ colorLabel CONSTANT)
    Q_PROPERTY(QString absoluteLabel READ absoluteLabel CONSTANT)
    Q_PROPERTY(QString amountLabel READ amountLabel CONSTANT)
    Q_PROPERTY(QString modeLabel READ modeLabel CONSTANT)
    Q_PROPERTY(QString directionalLabel READ directionalLabel CONSTANT)
    Q_PROPERTY(QString blurXLabel READ blurXLabel CONSTANT)
    Q_PROPERTY(QString blurYLabel READ blurYLabel CONSTANT)
    Q_PROPERTY(QString angleLabel READ angleLabel CONSTANT)

    Q_PROPERTY(int hue READ hue NOTIFY changed)
    Q_PROPERTY(int saturation READ saturation NOTIFY changed)
    Q_PROPERTY(int value READ value NOTIFY changed)
    Q_PROPERTY(bool absolute READ absolute NOTIFY changed)
    Q_PROPERTY(double amount READ amount NOTIFY changed)
    Q_PROPERTY(bool directional READ directional NOTIFY changed)
    Q_PROPERTY(double blurX READ blurX NOTIFY changed)
    Q_PROPERTY(double blurY READ blurY NOTIFY changed)
    Q_PROPERTY(double blurAngle READ blurAngle NOTIFY changed)

public:
    FilterRowModel(core::TimeKeyType aType, InspectorViewModel& aOwner, QObject* aParent = nullptr);

    int type() const { return int(mType); }
    QString kind() const;
    QString name() const;
    bool enabled() const { return mEnabled; }
    bool expanded() const { return mExpanded; }
    QString keyState() const { return mKeyState; }
    QColor keyColor() const { return mKeyColor; }
    QString summary() const { return mSummary; }
    QVariantList summarySegments() const { return mSummarySegments; }
    QString hueLabel() const { return QCoreApplication::translate("gui::prop::FilterStackPanel", "Hue"); }
    QString saturationLabel() const { return QCoreApplication::translate("gui::prop::FilterStackPanel", "Saturation"); }
    QString valueLabel() const { return QCoreApplication::translate("gui::prop::FilterStackPanel", "Value"); }
    QString colorLabel() const { return QCoreApplication::translate("gui::prop::FilterStackPanel", "Color"); }
    QString absoluteLabel() const { return QCoreApplication::translate("gui::prop::FilterStackPanel", "Absolute"); }
    QString amountLabel() const { return QCoreApplication::translate("gui::prop::FilterStackPanel", "Amount"); }
    QString modeLabel() const { return QCoreApplication::translate("gui::prop::FilterStackPanel", "Mode"); }
    QString directionalLabel() const { return QCoreApplication::translate("gui::prop::FilterStackPanel", "Directional"); }
    QString blurXLabel() const { return QCoreApplication::translate("gui::prop::FilterStackPanel", "Blur X"); }
    QString blurYLabel() const { return QCoreApplication::translate("gui::prop::FilterStackPanel", "Blur Y"); }
    QString angleLabel() const { return QCoreApplication::translate("gui::prop::FilterStackPanel", "Angle"); }

    int hue() const { return mHue; }
    int saturation() const { return mSaturation; }
    int value() const { return mValue; }
    bool absolute() const { return mAbsolute; }
    double amount() const { return mAmount; }
    bool directional() const { return mDirectional; }
    double blurX() const { return mBlurX; }
    double blurY() const { return mBlurY; }
    double blurAngle() const { return mBlurAngle; }

    void refresh();
    void refreshTheme();
    void setExpanded(bool aExpanded);

    Q_INVOKABLE void toggleExpanded();
    Q_INVOKABLE void toggleKey();
    Q_INVOKABLE void toggleEnabled();
    Q_INVOKABLE void remove();

    Q_INVOKABLE void setHue(int aValue);
    Q_INVOKABLE void setSaturation(int aValue);
    Q_INVOKABLE void setValue(int aValue);
    Q_INVOKABLE void setAbsolute(bool aValue);
    Q_INVOKABLE void setAmount(double aValue);
    Q_INVOKABLE void setDirectional(bool aValue);
    Q_INVOKABLE void setBlurX(double aValue);
    Q_INVOKABLE void setBlurY(double aValue);
    Q_INVOKABLE void setBlurAngle(double aValue);

signals:
    void changed();

private:
    bool isHSV() const { return mType == core::TimeKeyType_HSV; }
    bool isBlur() const { return mType == core::TimeKeyType_Blur; }
    void writeBlurEllipse();

    InspectorViewModel& mOwner;
    core::TimeKeyType mType;
    bool mEnabled = true;
    bool mExpanded = false;
    QString mKeyState;
    QColor mKeyColor;
    QString mSummary;
    QVariantList mSummarySegments;

    int mHue = 0;
    int mSaturation = 100;
    int mValue = 100;
    bool mAbsolute = false;
    double mAmount = 0.0;
    bool mDirectional = false;
    double mBlurX = 0.0;
    double mBlurY = 0.0;
    double mBlurAngle = 0.0;
};

class FilterStackModel: public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(QVariantList catalog READ catalog CONSTANT)
    Q_PROPERTY(QString noFiltersText READ noFiltersText CONSTANT)
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(QString addLabel READ addLabel CONSTANT)
    Q_PROPERTY(QString searchLabel READ searchLabel CONSTANT)
    Q_PROPERTY(QString noMatchLabel READ noMatchLabel CONSTANT)
    Q_PROPERTY(QString addedLabel READ addedLabel CONSTANT)
    Q_PROPERTY(QString comingSoonLabel READ comingSoonLabel CONSTANT)

public:
    explicit FilterStackModel(InspectorViewModel& aOwner, QObject* aParent = nullptr);

    QVariantList rows() const;
    QVariantList catalog() const;
    QString noFiltersText() const;
    QString title() const;
    QString addLabel() const;
    QString searchLabel() const;
    QString noMatchLabel() const;
    QString addedLabel() const;
    QString comingSoonLabel() const;

    void setProject(core::Project* aProject);
    void setTarget(core::ObjectNode* aTarget);
    void refresh();
    void refreshTheme();

    Q_INVOKABLE bool hasFilter(int aType) const;
    Q_INVOKABLE void addFilter(int aType);
    Q_INVOKABLE void removeFilter(int aType);
    Q_INVOKABLE void moveFilter(int aType, int aTargetIndex);
    Q_INVOKABLE void setFilterEnabled(int aType, bool aEnabled);
    Q_INVOKABLE void toggleExpanded(int aType);

    core::TimeKeyType selected() const { return mSelected; }
    void clearExpansion();
    FilterRowModel* rowAt(int aIndex) const {
        return (0 <= aIndex && aIndex < mRows.size()) ? mRows.at(aIndex) : nullptr;
    }

signals:
    void rowsChanged();

private:
    friend class FilterRowModel;
    FilterRowModel* rowFor(core::TimeKeyType aType) const;
    void rebuildRows(const QVector<core::FilterEntry>& aEntries);

    InspectorViewModel& mOwner;
    core::Project* mProject = nullptr;
    core::ObjectNode* mTarget = nullptr;
    QVector<FilterRowModel*> mRows;
    core::TimeKeyType mSelected = core::TimeKeyType_TERM;
};

} // namespace gui

#endif // GUI_QML_FILTERSTACKMODEL_H
