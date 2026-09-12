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
    // Image::hasRawAlpha for why it is not "anything nonzero", and for the open problem that a
    // fixed value is wrong in both directions.
    // The DEFAULT value of the content test below. Not a constant of the algorithm: the test is
    // passed in (GridMesh::createFromImage), so an experiment or a future per-layer derivation can
    // move it without a rebuild-time edit. Production passes nothing and gets this value.
    enum { kContentAlphaThreshold = 10 };
    // The alpha above which a pixel is treated as "ink the layer draws" when a boundary vertex
    // decides how far it may be pulled inward (Image::hasEdgeAlpha). Deliberately not the content
    // threshold: see hasEdgeAlpha.
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
        // "Is this pixel part of the PICTURE?" - the question the whole mesh is built from. A
        // cell exists only where this is true, and the burr reduction pulls the boundary vertices
        // inward until they stop seeing it, so THIS TEST DECIDES THE MESH'S SILHOUETTE.
        //
        // That is why it must not be "alpha != 0". The mesh is the geometry the artist deforms,
        // and it has to follow the shape they are drawing, not everything the file happens to
        // contain. Painting leaves two very different kinds of faint pixel:
        //
        //   * deliberate paint at a low level - an airbrushed shadow, a soft glow's skirt. The
        //     artist chose that level; it is part of the form.
        //   * accidents - the residue an erase tool leaves where the artist thought it erased
        //     everything (common with Flow < 100 %), and stray pixels from any partial operation.
        //     Nobody chose them, and they are orders of magnitude fainter than any deliberate
        //     paint.
        //
        // With "alpha != 0" the accidents become content: they create cells, they push the burr
        // reduction outward, and the mesh silhouette bends towards a blob the artist never drew.
        // That is an AUTHORING defect, not a rendering cost - the artist then deforms geometry
        // whose shape does not match the form they are animating - and it is invisible in a
        // render, which is why a render-based measurement (a difference against the PSD's own
        // composite, in the revision that introduced this regression) does not see it. The
        // threshold is what keeps the accidents out; it was never a rendering optimisation.
        //
        // The value 10 is a guess, and the honest statement of the open problem is that a fixed
        // value is wrong in both directions: too high and it cuts the skirt of a deliberate soft
        // glow (real content, which the artist painted), too low and accidents get in. The
        // intended fix is to DERIVE it per layer from the layer's own alpha statistics (the
        // mean/max over pixels above 1, and the ratio of 2..10 pixels to pixels above 10), so
        // that a layer with genuine soft paint is treated differently from a layer with erase
        // residue. That is not implemented; until it is, this constant is the behaviour, and it
        // is deliberately not "anything nonzero".
        bool hasRawAlpha(int aX, int aY) const {
            return mData[(aX + aY * mSize.width()) * 4 + 3] > mContentAlphaThreshold;
        }
        bool hasAlpha(int aX, int aY) const {
            if (aX < 0 || mSize.width() <= aX || aY < 0 || mSize.height() <= aY)
                return false;
            return hasRawAlpha(aX, aY);
        }
        // "Would the layer DRAW something here?" - a question the content test above must not answer,
        // and the one a boundary vertex has to ask before its pull stops (see reduceBurrs, which also
        // walks the pull instead of testing its endpoint, the larger half of the same fix).
        //
        // The distinction matters when a pull ends on ink fainter than the content threshold: the
        // content test calls that empty, so the ladder stops shortening and leaves a longer pull,
        // which trims the mesh past pixels the layer does draw. Measured on 000031 of the corpus, the
        // pull of the vertex next to the opaque band swept over alpha 1 and the endpoint window held
        // nothing above the threshold; with this predicate the endpoint counts as ink and the pull
        // keeps shortening. On its own that change moves that file's whole-canvas difference against
        // its own stored composite from mean 0.2993 to 0.2701 (and the pixels differing from the
        // no-cutoff render by more than 8 from 237 to 109); the sweep is what fixes the opaque rows
        // themselves (0.2701 -> 0.2696, 109 -> 44 together with it).
        //
        // Which cells exist is the artist's rule; where an existing cell may stop trimming is a
        // rendering invariant, and the answer is "wherever the layer still has ink".
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
