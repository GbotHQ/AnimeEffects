#ifndef BEZIERCURVEEDITOR_H
#define BEZIERCURVEEDITOR_H

#include "util/Easing.h"
#include <QPen>
#include <QDoubleSpinBox>
#include <QWidget>

class BezierCurveEditor : public QWidget
{
    Q_OBJECT
public:
    explicit BezierCurveEditor(QWidget *parent = nullptr, util::Easing::CubicBezier* cubicBezier = nullptr, QVector<QDoubleSpinBox*> spins = {}, int* pro = nullptr);
    ~BezierCurveEditor();
    util::Easing::CubicBezier* bezier;
    QVector<QDoubleSpinBox*> spinBoxes;
    int* progress;

    void paintEvent(QPaintEvent *);
    void resizeEvent(QResizeEvent *);
    void mousePressEvent(QMouseEvent *event);
    void mouseMoveEvent(QMouseEvent *);
    void mouseReleaseEvent(QMouseEvent *);
    void leaveEvent(QEvent *);

    signals:

    public slots:

    public:
    enum PointIndices {
        StartPoint = 0,
        ControlPoint1 = 1,
        ControlPoint2 = 2,
        EndPoint = 3
    };

    //Functions
private:
    qreal distance(QPointF a, QPointF b) const {
        QPointF diff = a - b;
        return sqrt(diff.x()*diff.x() + diff.y()*diff.y());
    }

    // Which tangent handle (ControlPoint1/2) is under the cursor, or -1.
    int handleAt(const QPoint& aPos) const;

public:
    const int NUM_POINTS = 4;
    const qreal POINT_RADIUS = 6.0; // visual radius of the tangent handles
    const qreal HIT_RADIUS = 12.0;  // grab radius: generous like timeline keys

    QPointF         m_points[4];

    QPen        m_curvePen;

    bool        m_dragging;
    int         m_selectedPoint{};
    int         m_hoveredPoint = -1;
};

#endif // BEZIERCURVEEDITOR_H
