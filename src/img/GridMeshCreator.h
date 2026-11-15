#ifndef IMG_GRIDMESHCREATOR_H
#define IMG_GRIDMESHCREATOR_H

#include <QScopedArrayPointer>
#include <QSize>
#include <QRect>
#include <QtOpenGL/QtOpenGL>
#include "XC.h"
#include "img/Buffer.h"

namespace img {

class GridMeshCreator {
public:
    // The content test's threshold: a pixel counts as content when its alpha is ABOVE this. See
    // Image::hasRawAlpha for why it is not "anything nonzero", and for why a fixed value is wrong
    // in both directions.
    // This is the DEFAULT the creator is constructed with when a caller has no level of its own,
    // and in production it is the derivation's CEILING: MeshContentLevel clamps what it derives to
    // 1..kContentAlphaThreshold and falls back to this value when there is nothing to measure, so a
    // layer never gets a stricter level than this one. GridMesh::createFromImage defaults to
    // img::MeshContentLevel::kDerive, which is what passes the derived value here.
    enum { kContentAlphaThreshold = 10 };
    // The alpha above which a pixel counts as something the layer DRAWS, for the one question
    // a boundary vertex asks about its own pull (see Image::hasEdgeAlpha). Zero: any nonzero alpha.
    // Not kContentAlphaThreshold, for the reason measured there.
    enum { kEdgeAlphaThreshold = 0 };
    struct HexaConnection {
        int id[6];
        bool has(int aIndex) const { return id[aIndex] != -1; }
        void clear();
    };

    static int getCellTableCount(const QSize& aImageSize, int aCellWidth);

    GridMeshCreator(const uint8* aPtr, const QSize& aSize, int aCellPx,
                    int aContentAlphaThreshold = kContentAlphaThreshold);

    int vertexCount() const { return mVertexCount; }
    int indexCount() const { return mIndexCount; }
    QRect vertexRect() const;

    void writeVertices(GLfloat* aPosVec3, GLfloat* aTexVec2);
    void writeIndices(GLuint* aIndices);
    void writeConnections(HexaConnection* aDest);

private:
    struct Vertex {
        bool isExist;
        float x;
        float y;
        GLuint index;
        sint32 cell[6]; // clockwise from right side
        QVector2D reduceVec;
        float maxReduce;
        float reduceRate;

        QVector2D pos() const { return QVector2D(x, y); }
        QVector2D posReduced() const { return QVector2D(x, y) + (reduceVec * maxReduce * reduceRate); }
    };

    struct Cell {
        bool isExist;
        bool inverted;
        bool nonReducing;
        bool reducingFixed;
        float x;
        float y;
        Vertex* vtx[3];
    };

    class Image {
        img::Buffer mBuffer;
        const uint8* mData;
        QSize mSize;
        int mContentAlphaThreshold;

    public:
        Image(const uint8* aPtr, const QSize& aSize,
              int aContentAlphaThreshold = GridMeshCreator::kContentAlphaThreshold);

        QSize size() const { return mSize; }
        // "Is this pixel part of the PICTURE?" - the question the whole mesh is built from: a
        // cell exists only where at least one pixel passes this test, so this test decides which
        // cells the mesh is made of and thus its silhouette. The boundary pull that trims an
        // existing cell asks a different question and has its own, looser threshold
        // (hasEdgeAlpha): a cell that exists draws its whole texture, so trimming must stop at
        // any ink, not only at content.
        //
        // That is why it must not be "alpha != 0". The mesh is the geometry the artist deforms,
        // and it has to follow the shape they are drawing, not everything the file happens to
        // contain. Painting leaves two very different kinds of faint pixel:
        //
        //   * deliberate paint at a low level - an airbrushed shadow, a soft glow's skirt. The
        //     artist chose that level; it is part of the form.
        //   * accidents - the residue an erase tool leaves where the artist thought it erased
        //     everything (common with Flow < 100 %), and stray pixels from any partial operation.
        //     Nobody chose them, and they are usually far fainter than deliberate paint - though
        //     the two ranges overlap at the bottom, which is the difficulty this level exists to
        //     handle.
        //
        // With "alpha != 0" the accidents become content: they create cells, they push the burr
        // reduction outward, and the mesh silhouette bends towards a blob the artist never drew.
        // That is an AUTHORING defect, not a rendering cost - the artist then deforms geometry
        // whose shape does not match the form they are animating - and it barely moves a
        // difference against the PSD's own composite, because the extra pixels are faint; that is
        // why a composite difference did not catch the regression that introduced this test. The
        // threshold is what keeps the accidents out; it was never a rendering optimisation.
        //
        // The value 10 is the ceiling of the derived level; a fixed level is wrong in both
        // directions: too high and it cuts the skirt of a deliberate soft glow (real content,
        // which the artist painted), too low and accidents get in. img::MeshContentLevel derives
        // the level per layer from the layer's own alpha statistics (the mean over pixels above 2,
        // at a fraction of it) so that a layer with genuine soft paint is treated differently from
        // a layer with erase residue; read that header for the rule, and
        // tools/psd_composite_probe/README.md (Decision on the content level) for the corpus
        // comparison that chose it. What matters HERE is that the level is a parameter with one
        // meaning, not that 10 is the right number.
        bool hasRawAlpha(int aX, int aY) const {
            return mData[(aX + aY * mSize.width()) * 4 + 3] > mContentAlphaThreshold;
        }
        bool hasAlpha(int aX, int aY) const {
            if (aX < 0 || mSize.width() <= aX || aY < 0 || mSize.height() <= aY)
                return false;
            return hasRawAlpha(aX, aY);
        }
        // "Is there anything here the layer would DRAW?" - the question a boundary vertex asks
        // about its pull (see reduceBurrs, which walks the pull instead of testing its endpoint).
        //
        // A DIFFERENT question from the content test above, and it has its own threshold on
        // purpose: a pixel inside an existing cell is drawn at whatever alpha it has, so a pull may
        // not step over even alpha 1, while the cells themselves exist only where the content test
        // says the artist painted something. Two levels, each answering its own question.
        //
        // What that buys, measured on tools/render_verify's fixtures: S22's deliberate ramp keeps its
        // faint end (alpha 1..3 on a layer whose level is 4 - the level alone cuts 35 px of it), and
        // S25 keeps two pixels where the layer sits on a half-pixel placement next to alpha-4
        // residue (the quad itself draws 129 there). What it costs, measured over the 48 layers of
        // the corpus file 000035 with the level held at 10: the mesh covers 1 243 658 more pixels
        // (1.22 %) than the content test alone would, 1 648 459 of the covered pixels being in the
        // 0 < alpha <= 10 band against 497 840 - i.e. the extra geometry is nearly all over paint
        // the artist cannot see, and 55 pixels of it are above the level. Both numbers are
        // geometry; neither is a rendering difference against the PSD's own composite, which this
        // class's content test cannot be judged by (see hasRawAlpha above).
        bool hasEdgeAlpha(int aX, int aY) const {
            if (aX < 0 || mSize.width() <= aX || aY < 0 || mSize.height() <= aY)
                return false;
            return mData[(aX + aY * mSize.width()) * 4 + 3] > kEdgeAlphaThreshold;
        }
        bool hasSomeAlphaIn3x3(int aX, int aY) const;
        bool getOpaExistence(const Cell& aCell, const QSizeF& aCellSize) const;
    };

    class VertexTable {
        QScopedArrayPointer<Vertex> mVertices;
        QSize mSize;
        const float mHalfSqrt3;

    public:
        VertexTable(int aWidth, int aHeight);

        int width() const { return mSize.width(); }
        int height() const { return mSize.height(); }

        Vertex& vertex(int aX, int aY) {
            XC_MSG_ASSERT(0 <= aX && aX < mSize.width(), "x=%d", aX);
            XC_MSG_ASSERT(0 <= aY && aY < mSize.height(), "y=%d", aY);
            return mVertices[aX + aY * mSize.width()];
        }

        const Vertex& vertex(int aX, int aY) const {
            XC_MSG_ASSERT(0 <= aX && aX < mSize.width(), "x=%d", aX);
            XC_MSG_ASSERT(0 <= aY && aY < mSize.height(), "y=%d", aY);
            return mVertices[aX + aY * mSize.width()];
        }

        const Vertex* findVertex(int aX, int aY) const {
            if (aX < 0 || mSize.width() <= aX)
                return nullptr;
            if (aY < 0 || mSize.height() <= aY)
                return nullptr;
            return &(mVertices[aX + aY * mSize.width()]);
        }

        void setExistance(bool aValue, int aX, int aY) {
            if (aX < 0 || mSize.width() <= aX)
                return;
            if (aY < 0 || mSize.height() <= aY)
                return;
            mVertices[aX + aY * mSize.width()].isExist = aValue;
        }

        void setReducingVectors(float aCellWidth);
        void shortenReducingVectorsOnePixel();
    };

    class CellTable {
        QScopedArrayPointer<Cell> mCells;
        QSizeF mCellSize;
        int mWidth;
        int mHeight;

    public:
        static QSizeF calculateCellSize(int aCellWidth);
        static QSize calculateCellTableSize(const QSize& aImageSize, const QSizeF& aCellSize);

        CellTable(int aCellWidth);
        int initCells(const Image& aImage);
        void connectCellsToVertices(VertexTable& aTable);
        Cell& cell(int aX, int aY);
        Cell* findExistingCell(int aX, int aY);
        // The cell that owns a pixel, or nullptr when none of the candidates exists. Used when
        // a content pixel is covered by no cell at all, where the cell's own index is not
        // known (see the reduction loop in reduceBurrs). Row y of the table starts at
        // cellHeight * y and a cell spans halfCellWidth * (x - 1) .. halfCellWidth * (x + 1),
        // so the containing column is derived from the pixel and the neighbours are checked
        // too (the rows are offset from each other).
        Cell* cellAtPixel(int aPixX, int aPixY);
        QSizeF cellSize() const { return mCellSize; }
        float cellWidth() const { return mCellSize.width(); }
        float cellHeight() const { return mCellSize.height(); }
        int tableWidth() const { return mWidth; }
        int tableHeight() const { return mHeight; }
    };

    void execute(const uint8* aPtr, const QSize& aSize, int aCellPx);
    void initPositionsOfVertices(VertexTable& aTable);
    const Vertex* findConnectVertex(int aX, int aY, int aConnectId) const;
    void setIndicesOfExistingVertices(VertexTable& aTable);
    void reduceBurrs(VertexTable& aTable, const Image& aImage);

    QScopedPointer<CellTable> mCells;
    QScopedPointer<VertexTable> mVertices;
    int mVertexCount;
    int mIndexCount;
    QSize mImageSize;
    int mContentAlphaThreshold;
};

} // namespace img

#endif // IMG_GRIDMESHCREATOR_H
