#ifndef GUI_QML_INSPECTORVIEWMODEL_H
#define GUI_QML_INSPECTORVIEWMODEL_H

#include <QColor>
#include <QObject>
#include <QVariantList>
#include <QVector>

#include "core/ObjectNode.h"
#include "core/Project.h"
#include "gui/prop/prop_KeyAccessor.h"

namespace gui {

class InspectorRowModel;
class FilterStackModel;
class ViaPoint;

// The widget-free model behind PropertiesPanel.qml. It is deliberately not a
// QAbstractItemModel: the inspector is a fixed set of rows plus one stack, and
// exposing the two as QObject graphs keeps each row's typed values visible to
// QML without role plumbing.
class InspectorViewModel: public QObject {
    Q_OBJECT

    Q_PROPERTY(InspectorRowModel* move READ move CONSTANT)
    Q_PROPERTY(InspectorRowModel* rotate READ rotate CONSTANT)
    Q_PROPERTY(InspectorRowModel* scale READ scale CONSTANT)
    Q_PROPERTY(InspectorRowModel* depth READ depth CONSTANT)
    Q_PROPERTY(InspectorRowModel* opacity READ opacity CONSTANT)
    Q_PROPERTY(InspectorRowModel* image READ image CONSTANT)
    Q_PROPERTY(InspectorRowModel* pose READ pose CONSTANT)
    Q_PROPERTY(InspectorRowModel* ffd READ ffd CONSTANT)
    Q_PROPERTY(FilterStackModel* filters READ filters CONSTANT)

    Q_PROPERTY(bool hasTarget READ hasTarget NOTIFY changed)
    Q_PROPERTY(bool playbackActive READ playbackActive NOTIFY changed)

    Q_PROPERTY(QString blendLabel READ blendLabel CONSTANT)
    Q_PROPERTY(QString clippedLabel READ clippedLabel CONSTANT)
    Q_PROPERTY(QString centroidLabel READ centroidLabel CONSTANT)
    Q_PROPERTY(QString pathLabel READ pathLabel CONSTANT)
    Q_PROPERTY(QString smoothLabel READ smoothLabel CONSTANT)
    Q_PROPERTY(QString smoothTooltip READ smoothTooltip CONSTANT)
    Q_PROPERTY(QString centerLabel READ centerLabel CONSTANT)
    Q_PROPERTY(QString meshCellLabel READ meshCellLabel CONSTANT)
    Q_PROPERTY(QString meshCellTooltip READ meshCellTooltip CONSTANT)
    Q_PROPERTY(QVariantList blendOptions READ blendOptions CONSTANT)
    Q_PROPERTY(int blendIndex READ blendIndex NOTIFY changed)
    Q_PROPERTY(bool blendEnabled READ blendEnabled NOTIFY changed)
    Q_PROPERTY(bool clipped READ clipped NOTIFY changed)
    Q_PROPERTY(bool clippedEnabled READ clippedEnabled NOTIFY changed)

    // Field ranges. QML must not invent its own limits: these are the same
    // constants the widget inspector used.
    Q_PROPERTY(double translateMin READ translateMin CONSTANT)
    Q_PROPERTY(double translateMax READ translateMax CONSTANT)
    Q_PROPERTY(double rotateMin READ rotateMin CONSTANT)
    Q_PROPERTY(double rotateMax READ rotateMax CONSTANT)
    Q_PROPERTY(double scaleMin READ scaleMin CONSTANT)
    Q_PROPERTY(double scaleMax READ scaleMax CONSTANT)
    Q_PROPERTY(int imageCellMin READ imageCellMin CONSTANT)
    Q_PROPERTY(int imageCellMax READ imageCellMax CONSTANT)
    Q_PROPERTY(double blurMax READ blurMax CONSTANT)
    Q_PROPERTY(double blurAngleMin READ blurAngleMin CONSTANT)
    Q_PROPERTY(double blurAngleMax READ blurAngleMax CONSTANT)
    Q_PROPERTY(int hsvHueMax READ hsvHueMax CONSTANT)
    Q_PROPERTY(int hsvComponentMax READ hsvComponentMax CONSTANT)

public:
    explicit InspectorViewModel(ViaPoint* aViaPoint, QObject* aParent = nullptr);

    void setProject(core::Project* aProject);
    void setTarget(core::ObjectNode* aTarget);
    void setPlaybackActive(bool aActive);
    void refresh();
    void refreshAttributes();
    void refreshKeys();
    void refreshFrame();
    void refreshTheme();

    InspectorRowModel* move() const { return mMove; }
    InspectorRowModel* rotate() const { return mRotate; }
    InspectorRowModel* scale() const { return mScale; }
    InspectorRowModel* depth() const { return mDepth; }
    InspectorRowModel* opacity() const { return mOpacity; }
    InspectorRowModel* image() const { return mImage; }
    InspectorRowModel* pose() const { return mPose; }
    InspectorRowModel* ffd() const { return mFFD; }
    FilterStackModel* filters() const { return mFilters; }

    bool hasTarget() const { return mProject && mTarget && mTarget->timeLine(); }
    bool playbackActive() const { return mPlaybackActive; }
    core::TimeLine* timeLine() const;
    int frame() const;
    gui::prop::KeyAccessor& accessor() { return mAccessor; }
    core::Project* project() const { return mProject; }
    core::ObjectNode* target() const { return mTarget; }

    QString blendLabel() const;
    QString clippedLabel() const;
    QString centroidLabel() const;
    QString pathLabel() const;
    QString smoothLabel() const;
    QString smoothTooltip() const;
    QString centerLabel() const;
    QString meshCellLabel() const;
    QString meshCellTooltip() const;
    QVariantList blendOptions() const { return mBlendOptions; }
    int blendIndex() const { return mBlendIndex; }
    bool blendEnabled() const { return mBlendEnabled; }
    bool clipped() const { return mClipped; }
    bool clippedEnabled() const { return mClippedEnabled; }

    double translateMin() const;
    double translateMax() const;
    double rotateMin() const;
    double rotateMax() const;
    double scaleMin() const;
    double scaleMax() const;
    int imageCellMin() const;
    int imageCellMax() const;
    double blurMax() const;
    double blurAngleMin() const;
    double blurAngleMax() const;
    int hsvHueMax() const;
    int hsvComponentMax() const;

    Q_INVOKABLE void setBlendIndex(int aIndex);
    Q_INVOKABLE void setClipped(bool aValue);
    Q_INVOKABLE void browseImageAsset();
    Q_INVOKABLE void addImageKey();

signals:
    void changed();

private:
    friend class InspectorRowModel;
    friend class FilterRowModel;
    friend class FilterStackModel;

    void updateConstants();
    void assignBlend(img::BlendMode aMode);

    ViaPoint* mViaPoint;
    core::Project* mProject = nullptr;
    core::ObjectNode* mTarget = nullptr;
    bool mPlaybackActive = false;
    gui::prop::KeyAccessor mAccessor;

    InspectorRowModel* mMove = nullptr;
    InspectorRowModel* mRotate = nullptr;
    InspectorRowModel* mScale = nullptr;
    InspectorRowModel* mDepth = nullptr;
    InspectorRowModel* mOpacity = nullptr;
    InspectorRowModel* mImage = nullptr;
    InspectorRowModel* mPose = nullptr;
    InspectorRowModel* mFFD = nullptr;
    FilterStackModel* mFilters = nullptr;

    QVariantList mBlendOptions;
    int mBlendIndex = 0;
    bool mBlendEnabled = false;
    bool mClipped = false;
    bool mClippedEnabled = false;
};

} // namespace gui

#endif // GUI_QML_INSPECTORVIEWMODEL_H
