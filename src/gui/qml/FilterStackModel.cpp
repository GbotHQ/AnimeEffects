#include "gui/qml/FilterStackModel.h"

#include <QColor>
#include <QCoreApplication>
#include <QVariant>

#include "core/BlurKey.h"
#include "core/Constant.h"
#include "core/FilterFrame.h"
#include "core/HsvKey.h"
#include "core/TimeKeyExpans.h"
#include "ctrl/TimeLineUtil.h"
#include "gui/qml/InspectorViewModel.h"
#include "gui/theme/Colors.h"

namespace gui {

namespace {
QString trFilter(const char* aText) {
    return QCoreApplication::translate("gui::prop::FilterStackPanel", aText);
}

QString displayName(core::TimeKeyType aType) {
    switch (aType) {
    case core::TimeKeyType_HSV: return trFilter("HSV");
    case core::TimeKeyType_Blur: return trFilter("Blur");
    default: return QString();
    }
}

bool isStackFilter(core::TimeKeyType aType) {
    return aType == core::TimeKeyType_HSV || aType == core::TimeKeyType_Blur;
}

// Same display rule as ScrubSpinBox's trimTrailingZeros: the summary must not
// show 0.0 when the field shows 0.
QString summaryNumber(double aValue, int aDecimals) {
    QString text = QString::number(aValue, 'f', aDecimals);
    if (text.contains('.')) {
        while (text.endsWith('0'))
            text.chop(1);
        if (text.endsWith('.'))
            text.chop(1);
    }
    return text == QStringLiteral("-0") ? QStringLiteral("0") : text;
}
} // namespace

FilterRowModel::FilterRowModel(core::TimeKeyType aType, InspectorViewModel& aOwner, QObject* aParent):
    QObject(aParent), mOwner(aOwner), mType(aType) {}

QString FilterRowModel::kind() const {
    if (isHSV()) return QStringLiteral("hsv");
    if (isBlur()) return QStringLiteral("blur");
    return QString();
}

QString FilterRowModel::name() const { return displayName(mType); }

void FilterRowModel::refresh() {
    core::TimeLine* line = mOwner.timeLine();
    if (!line)
        return;

    const int frame = mOwner.frame();
    const bool enabled = line->filterEnabled(mType);
    QString keyState;
    if (line->map(mType).isEmpty())
        keyState = QStringLiteral("static");
    else if (line->hasTimeKey(mType, frame))
        keyState = QStringLiteral("keyed");
    else
        keyState = QStringLiteral("animated");

    const QColor keyColor = theme::keyTypeColor(theme::Colors::current(), mType);
    const QVariantList before{mEnabled, mKeyState, mKeyColor, mSummary, mSummarySegments, mHue,
                              mSaturation, mValue, mAbsolute, mAmount, mDirectional, mBlurX,
                              mBlurY, mBlurAngle};
    QVariantList summarySegments;
    const auto addSummary = [&summarySegments](const QString& aLabel, const QString& aValue) {
        QVariantMap segment;
        segment.insert(QStringLiteral("label"), aLabel);
        segment.insert(QStringLiteral("value"), aValue);
        summarySegments.append(segment);
    };

    mEnabled = enabled;
    mKeyState = keyState;
    mKeyColor = keyColor;

    const core::TimeKeyExpans& current = line->current();
    if (isHSV()) {
        const QList<int> values = current.hsv().hsv();
        mHue = values.size() > 0 ? values.at(0) : 0;
        mSaturation = values.size() > 1 ? values.at(1) : 100;
        mValue = values.size() > 2 ? values.at(2) : 100;
        mAbsolute = values.size() > 3 && values.at(3) != 0;
        mSummary = trFilter("H %1  S %2  V %3").arg(mHue).arg(mSaturation).arg(mValue);
        addSummary(QStringLiteral("H"), QString::number(mHue));
        addSummary(QStringLiteral("S"), QString::number(mSaturation));
        addSummary(QStringLiteral("V"), QString::number(mValue));
    } else if (isBlur()) {
        const auto data = mOwner.accessor().blurData();
        const bool hasExplicit = line->hasTimeKey(mType, frame);
        const auto* key = hasExplicit ? line->timeKey(mType, frame) : line->defaultKey(mType);
        // Between keys the live radii are what the frame plays; the mode flag
        // belongs to a stored key, so it falls back to linear when this frame
        // is interpolated.
        mDirectional = key ? static_cast<const core::BlurKey*>(key)->data().isDirectional() : false;
        mAmount = current.blurX();
        mBlurX = current.blurX();
        mBlurY = current.blurY();
        mBlurAngle = current.angleDeg();
        if (data.amount() != mAmount) // keep the accessor's exact read for a static default
            mAmount = data.amount();
        const QString blurX = summaryNumber(double(mBlurX), 1);
        const QString blurY = summaryNumber(double(mBlurY), 1);
        const QString blurAngle = summaryNumber(double(mBlurAngle), 1);
        const QString amount = summaryNumber(double(mAmount), 1);
        mSummary = mDirectional
            ? trFilter("X %1 px  Y %2 px  Angle %3°").arg(blurX, blurY, blurAngle)
            : trFilter("Amount %1 px").arg(amount);
        if (mDirectional) {
            addSummary(QStringLiteral("X"), blurX + QStringLiteral(" px"));
            addSummary(QStringLiteral("Y"), blurY + QStringLiteral(" px"));
            addSummary(trFilter("Angle"), blurAngle + QStringLiteral("°"));
        } else {
            addSummary(trFilter("Amount"), amount + QStringLiteral(" px"));
        }
    }
    mSummarySegments = summarySegments;

    const QVariantList after{mEnabled, mKeyState, mKeyColor, mSummary, mSummarySegments, mHue,
                             mSaturation, mValue, mAbsolute, mAmount, mDirectional, mBlurX,
                             mBlurY, mBlurAngle};
    if (before != after)
        emit changed();
}

void FilterRowModel::refreshTheme() {
    const QColor color = theme::keyTypeColor(theme::Colors::current(), mType);
    if (color != mKeyColor) {
        mKeyColor = color;
        emit changed();
    }
}

void FilterRowModel::setExpanded(bool aExpanded) {
    if (mExpanded == aExpanded)
        return;
    mExpanded = aExpanded;
    emit changed();
}

void FilterRowModel::toggleExpanded() { mOwner.filters()->toggleExpanded(int(mType)); }

void FilterRowModel::toggleKey() {
    core::TimeLine* line = mOwner.timeLine();
    if (!line)
        return;
    if (line->hasTimeKey(mType, mOwner.frame()))
        mOwner.accessor().removeKey(mType);
    else if (isHSV())
        mOwner.accessor().knockNewHSV();
    else if (isBlur())
        mOwner.accessor().knockNewBlur();
    mOwner.refreshKeys();
}

void FilterRowModel::toggleEnabled() {
    core::TimeLine* line = mOwner.timeLine();
    if (!line)
        return;
    mOwner.filters()->setFilterEnabled(int(mType), !line->filterEnabled(mType));
}

void FilterRowModel::remove() { mOwner.filters()->removeFilter(int(mType)); }

void FilterRowModel::setHue(int aValue) {
    if (!isHSV() || !mOwner.hasTarget()) return;
    mOwner.accessor().assignHSV(aValue, QStringLiteral("hue"));
    mOwner.refreshKeys();
}

void FilterRowModel::setSaturation(int aValue) {
    if (!isHSV() || !mOwner.hasTarget()) return;
    mOwner.accessor().assignHSV(aValue, QStringLiteral("sat"));
    mOwner.refreshKeys();
}

void FilterRowModel::setValue(int aValue) {
    if (!isHSV() || !mOwner.hasTarget()) return;
    mOwner.accessor().assignHSV(aValue, QStringLiteral("val"));
    mOwner.refreshKeys();
}

void FilterRowModel::setAbsolute(bool aValue) {
    if (!isHSV() || !mOwner.hasTarget()) return;
    mOwner.accessor().assignHSV(int(aValue), QStringLiteral("abs"));
    mOwner.refreshKeys();
}

void FilterRowModel::setAmount(double aValue) {
    if (!isBlur() || !mOwner.hasTarget()) return;
    mOwner.accessor().assignBlur(float(aValue));
    mOwner.refreshKeys();
}

void FilterRowModel::setDirectional(bool aValue) {
    if (!isBlur() || !mOwner.hasTarget()) return;
    mOwner.accessor().assignBlurDirectional(aValue);
    mOwner.refreshKeys();
}

void FilterRowModel::writeBlurEllipse() {
    if (!isBlur() || !mOwner.hasTarget()) return;
    mOwner.accessor().assignBlurEllipse(float(mBlurX), float(mBlurY), float(mBlurAngle));
    mOwner.refreshKeys();
}

void FilterRowModel::setBlurX(double aValue) {
    if (!isBlur()) return;
    mBlurX = aValue;
    writeBlurEllipse();
}

void FilterRowModel::setBlurY(double aValue) {
    if (!isBlur()) return;
    mBlurY = aValue;
    writeBlurEllipse();
}

void FilterRowModel::setBlurAngle(double aValue) {
    if (!isBlur()) return;
    mBlurAngle = aValue;
    writeBlurEllipse();
}

//-------------------------------------------------------------------------------------------------
FilterStackModel::FilterStackModel(InspectorViewModel& aOwner, QObject* aParent):
    QObject(aParent), mOwner(aOwner) {}

QVariantList FilterStackModel::rows() const {
    QVariantList out;
    for (FilterRowModel* row : mRows)
        out.append(QVariant::fromValue(row));
    return out;
}

QVariantList FilterStackModel::catalog() const {
    const auto entry = [](const QString& aName, const QString& aDescription, core::TimeKeyType aType,
                          bool aAvailable) {
        QVariantMap map;
        map.insert(QStringLiteral("name"), aName);
        map.insert(QStringLiteral("description"), aDescription);
        map.insert(QStringLiteral("type"), int(aType));
        map.insert(QStringLiteral("available"), aAvailable);
        return map;
    };
    return {
        entry(trFilter("HSV"), trFilter("Hue, saturation and value adjustment."),
              core::TimeKeyType_HSV, true),
        entry(trFilter("Blur"), trFilter("Directional Gaussian blur in the layer's content space."),
              core::TimeKeyType_Blur, true),
        entry(trFilter("Wave Deform"), trFilter("Traveling wave distortion. Coming soon."),
              core::TimeKeyType_TERM, false),
        entry(trFilter("Custom GLSL"), trFilter("A fragment shader with a guided, typed interface. Coming soon."),
              core::TimeKeyType_TERM, false),
    };
}

QString FilterStackModel::noFiltersText() const { return trFilter("No filters"); }
QString FilterStackModel::title() const { return trFilter("Filters"); }
QString FilterStackModel::addLabel() const { return trFilter("Add filter"); }
QString FilterStackModel::searchLabel() const { return trFilter("Search filters"); }
QString FilterStackModel::noMatchLabel() const { return trFilter("No matching filters"); }
QString FilterStackModel::addedLabel() const { return trFilter("Added"); }
QString FilterStackModel::comingSoonLabel() const { return trFilter("Coming soon"); }

void FilterStackModel::setProject(core::Project* aProject) {
    mProject = aProject;
    mSelected = core::TimeKeyType_TERM;
    refresh();
}

void FilterStackModel::setTarget(core::ObjectNode* aTarget) {
    mTarget = aTarget;
    mSelected = core::TimeKeyType_TERM;
    refresh();
}

FilterRowModel* FilterStackModel::rowFor(core::TimeKeyType aType) const {
    for (FilterRowModel* row : mRows) {
        if (core::TimeKeyType(row->type()) == aType)
            return row;
    }
    return nullptr;
}

void FilterStackModel::rebuildRows(const QVector<core::FilterEntry>& aEntries) {
    for (FilterRowModel* row : mRows)
        row->deleteLater();
    mRows.clear();

    bool selectedExists = false;
    for (const core::FilterEntry& entry : aEntries) {
        auto* row = new FilterRowModel(entry.type, mOwner, this);
        mRows.append(row);
        if (entry.type == mSelected)
            selectedExists = true;
    }
    if (!selectedExists)
        mSelected = aEntries.isEmpty() ? core::TimeKeyType_TERM : aEntries.front().type;
    emit rowsChanged();
}

void FilterStackModel::refresh() {
    core::TimeLine* line = mOwner.timeLine();
    if (!line) {
        if (!mRows.isEmpty()) {
            for (FilterRowModel* row : mRows)
                row->deleteLater();
            mRows.clear();
            mSelected = core::TimeKeyType_TERM;
            emit rowsChanged();
        }
        return;
    }

    const QVector<core::FilterEntry> entries = line->filters();
    bool structureChanged = entries.size() != mRows.size();
    if (!structureChanged) {
        for (int i = 0; i < entries.size(); ++i) {
            if (entries.at(i).type != core::TimeKeyType(mRows.at(i)->type())) {
                structureChanged = true;
                break;
            }
        }
    }
    if (structureChanged)
        rebuildRows(entries);

    // A user who collapsed every card keeps that state across frame changes.
    // Only a structural rebuild chooses a first card when nothing is selected.
    for (FilterRowModel* row : mRows) {
        row->refresh();
        row->setExpanded(core::TimeKeyType(row->type()) == mSelected);
    }
}

void FilterStackModel::refreshTheme() {
    for (FilterRowModel* row : mRows)
        row->refreshTheme();
}

bool FilterStackModel::hasFilter(int aType) const {
    core::TimeLine* line = mOwner.timeLine();
    return line && isStackFilter(core::TimeKeyType(aType)) && line->hasFilter(core::TimeKeyType(aType));
}

void FilterStackModel::addFilter(int aType) {
    const auto type = core::TimeKeyType(aType);
    if (!mProject || !mTarget || !isFilterType(type))
        return;
    ctrl::TimeLineUtil::addFilter(*mProject, *mTarget, type);
    mSelected = type;
    refresh();
}

void FilterStackModel::removeFilter(int aType) {
    const auto type = core::TimeKeyType(aType);
    if (!mProject || !mTarget || !isFilterType(type))
        return;
    ctrl::TimeLineUtil::removeFilter(*mProject, *mTarget, type);
    if (mSelected == type)
        mSelected = core::TimeKeyType_TERM;
    refresh();
}

void FilterStackModel::moveFilter(int aType, int aTargetIndex) {
    const auto type = core::TimeKeyType(aType);
    if (!mProject || !mTarget || !isFilterType(type))
        return;
    const int count = mRows.size();
    if (count < 2)
        return;
    ctrl::TimeLineUtil::moveFilter(*mProject, *mTarget, type, qBound(0, aTargetIndex, count - 1));
    refresh();
}

void FilterStackModel::setFilterEnabled(int aType, bool aEnabled) {
    const auto type = core::TimeKeyType(aType);
    if (!mProject || !mTarget || !isFilterType(type))
        return;
    ctrl::TimeLineUtil::setFilterEnabled(*mProject, *mTarget, type, aEnabled);
    refresh();
}

void FilterStackModel::toggleExpanded(int aType) {
    const auto type = core::TimeKeyType(aType);
    mSelected = mSelected == type ? core::TimeKeyType_TERM : type;
    for (FilterRowModel* row : mRows)
        row->setExpanded(core::TimeKeyType(row->type()) == mSelected);
}

void FilterStackModel::clearExpansion() {
    mSelected = core::TimeKeyType_TERM;
    for (FilterRowModel* row : mRows)
        row->setExpanded(false);
}

} // namespace gui
