/****************************************************************************
**
** Copyright (C) 2011 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** This file is part of the QtGui module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** No Commercial Usage
** This file contains pre-release code and may not be distributed.
** You may use this file in accordance with the terms and conditions
** contained in the Technology Preview License Agreement accompanying
** this package.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights.  These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** If you have questions regarding the use of this file, please contact
** Nokia at qt-info@nokia.com.
**
**
**
**
**
**
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#ifndef QT_NO_DIRECTWRITE

#include "qfontenginedirectwrite_p.h"

#include <qendian.h>
#include <dwrite.h>
#include <private/qnativeimage_p.h>

#include <d2d1.h>

QT_BEGIN_NAMESPACE

// Convert from design units to logical pixels
#define DESIGN_TO_LOGICAL(DESIGN_UNIT_VALUE) \
    QFixed::fromReal((qreal(DESIGN_UNIT_VALUE) / qreal(m_unitsPerEm)) * fontDef.pixelSize)

namespace {

    class GeometrySink: public IDWriteGeometrySink
    {
    public:
        GeometrySink(QPainterPath *path) : m_path(path), m_refCount(0)
        {
            Q_ASSERT(m_path != 0);
        }

        IFACEMETHOD_(void, AddBeziers)(const D2D1_BEZIER_SEGMENT *beziers, UINT bezierCount);
        IFACEMETHOD_(void, AddLines)(const D2D1_POINT_2F *points, UINT pointCount);
        IFACEMETHOD_(void, BeginFigure)(D2D1_POINT_2F startPoint, D2D1_FIGURE_BEGIN figureBegin);
        IFACEMETHOD(Close)();
        IFACEMETHOD_(void, EndFigure)(D2D1_FIGURE_END figureEnd);
        IFACEMETHOD_(void, SetFillMode)(D2D1_FILL_MODE fillMode);
        IFACEMETHOD_(void, SetSegmentFlags)(D2D1_PATH_SEGMENT vertexFlags);

        IFACEMETHOD_(unsigned long, AddRef)();
        IFACEMETHOD_(unsigned long, Release)();
        IFACEMETHOD(QueryInterface)(IID const &riid, void **ppvObject);

    private:
        inline static QPointF fromD2D1_POINT_2F(const D2D1_POINT_2F &inp)
        {
            return QPointF(inp.x, inp.y);
        }

        unsigned long m_refCount;
        QPointF m_startPoint;
        QPainterPath *m_path;
    };

    void GeometrySink::AddBeziers(const D2D1_BEZIER_SEGMENT *beziers,
                                  UINT bezierCount)
    {
        for (uint i=0; i<bezierCount; ++i) {
            QPointF c1 = fromD2D1_POINT_2F(beziers[i].point1);
            QPointF c2 = fromD2D1_POINT_2F(beziers[i].point2);
            QPointF p2 = fromD2D1_POINT_2F(beziers[i].point3);

            m_path->cubicTo(c1, c2, p2);
        }
    }

    void GeometrySink::AddLines(const D2D1_POINT_2F *points, UINT pointsCount)
    {
        for (uint i=0; i<pointsCount; ++i)
            m_path->lineTo(fromD2D1_POINT_2F(points[i]));
    }

    void GeometrySink::BeginFigure(D2D1_POINT_2F startPoint,
                                   D2D1_FIGURE_BEGIN /*figureBegin*/)
    {
        m_startPoint = fromD2D1_POINT_2F(startPoint);
        m_path->moveTo(m_startPoint);
    }

    IFACEMETHODIMP GeometrySink::Close()
    {
        return E_NOTIMPL;
    }

    void GeometrySink::EndFigure(D2D1_FIGURE_END figureEnd)
    {
        if (figureEnd == D2D1_FIGURE_END_CLOSED)
            m_path->closeSubpath();
    }

    void GeometrySink::SetFillMode(D2D1_FILL_MODE fillMode)
    {
        m_path->setFillRule(fillMode == D2D1_FILL_MODE_ALTERNATE
                            ? Qt::OddEvenFill
                            : Qt::WindingFill);
    }

    void GeometrySink::SetSegmentFlags(D2D1_PATH_SEGMENT /*vertexFlags*/)
    {
        /* Not implemented */
    }

    IFACEMETHODIMP_(unsigned long) GeometrySink::AddRef()
    {
        return InterlockedIncrement(&m_refCount);
    }

    IFACEMETHODIMP_(unsigned long) GeometrySink::Release()
    {
        unsigned long newCount = InterlockedDecrement(&m_refCount);
        if (newCount == 0)
        {
            delete this;
            return 0;
        }

        return newCount;
    }

    IFACEMETHODIMP GeometrySink::QueryInterface(IID const &riid, void **ppvObject)
    {
        if (__uuidof(IDWriteGeometrySink) == riid) {
            *ppvObject = this;
        } else if (__uuidof(IUnknown) == riid) {
            *ppvObject = this;
        } else {
            *ppvObject = NULL;
            return E_FAIL;
        }

        AddRef();
        return S_OK;
    }

}

QFontEngineDirectWrite::QFontEngineDirectWrite(const QString &name,
                                               IDWriteFactory *directWriteFactory,
                                               IDWriteGdiInterop *directWriteGdiInterop,
                                               IDWriteFont *directWriteFont,
                                               qreal pixelSize)
    : m_name(name)
    , m_directWriteFont(directWriteFont)
    , m_directWriteFontFace(0)
    , m_directWriteFactory(directWriteFactory)
    , m_directWriteBitmapRenderTarget(0)
    , m_directWriteGdiInterop(directWriteGdiInterop)
    , m_lineThickness(-1)
    , m_unitsPerEm(-1)
    , m_ascent(-1)
    , m_descent(-1)
    , m_xHeight(-1)
    , m_lineGap(-1)
{
    m_directWriteFont->AddRef();
    m_directWriteFactory->AddRef();
    m_directWriteGdiInterop->AddRef();

    fontDef.pixelSize = pixelSize;

    HRESULT hr = m_directWriteFont->CreateFontFace(&m_directWriteFontFace);
    if (FAILED(hr))
        qErrnoWarning("QFontEngineDirectWrite: CreateFontFace failed");

    collectMetrics();
}

QFontEngineDirectWrite::~QFontEngineDirectWrite()
{
    m_directWriteFont->Release();
    m_directWriteFactory->Release();
    m_directWriteGdiInterop->Release();

    if (m_directWriteBitmapRenderTarget != 0)
        m_directWriteBitmapRenderTarget->Release();
}

void QFontEngineDirectWrite::collectMetrics()
{
    if (m_directWriteFont != 0) {
        DWRITE_FONT_METRICS metrics;

        m_directWriteFont->GetMetrics(&metrics);
        m_unitsPerEm = metrics.designUnitsPerEm;

        m_lineThickness = DESIGN_TO_LOGICAL(metrics.underlineThickness);
        m_ascent = DESIGN_TO_LOGICAL(metrics.ascent);
        m_descent = DESIGN_TO_LOGICAL(metrics.descent);
        m_xHeight = DESIGN_TO_LOGICAL(metrics.xHeight);
        m_lineGap = DESIGN_TO_LOGICAL(metrics.lineGap);
    }
}

QFixed QFontEngineDirectWrite::lineThickness() const
{
    if (m_lineThickness > 0)
        return m_lineThickness;
    else
        return QFontEngine::lineThickness();
}

bool QFontEngineDirectWrite::getSfntTableData(uint tag, uchar *buffer, uint *length) const
{
    if (m_directWriteFontFace) {
        DWORD t = qbswap<quint32>(tag);

        const void *tableData = 0;
        void *tableContext = 0;
        UINT32 tableSize;
        BOOL exists;
        HRESULT hr = m_directWriteFontFace->TryGetFontTable(
                    t, &tableData, &tableSize, &tableContext, &exists
                    );

        if (SUCCEEDED(hr)) {
            if (!exists)
                return false;

            if (buffer == 0) {
                *length = tableSize;
                return true;
            } else if (*length < tableSize) {
                return false;
            }

            qMemCopy(buffer, tableData, tableSize);
            m_directWriteFontFace->ReleaseFontTable(tableContext);

            return true;
        } else {
            qErrnoWarning("QFontEngineDirectWrite::getSfntTableData: TryGetFontTable failed");
        }
    }

    return false;
}

QFixed QFontEngineDirectWrite::emSquareSize() const
{
    if (m_unitsPerEm > 0)
        return m_unitsPerEm;
    else
        return QFontEngine::emSquareSize();
}

inline unsigned int getChar(const QChar *str, int &i, const int len)
{
    unsigned int uc = str[i].unicode();
    if (uc >= 0xd800 && uc < 0xdc00 && i < len-1) {
        uint low = str[i+1].unicode();
       if (low >= 0xdc00 && low < 0xe000) {
            uc = (uc - 0xd800)*0x400 + (low - 0xdc00) + 0x10000;
            ++i;
        }
    }
    return uc;
}

bool QFontEngineDirectWrite::stringToCMap(const QChar *str, int len, QGlyphLayout *glyphs,
                                          int *nglyphs, QTextEngine::ShaperFlags flags) const
{
    if (m_directWriteFontFace != 0) {
        QVarLengthArray<UINT32> codePoints(len);
        for (int i=0; i<len; ++i) {
            codePoints[i] = getChar(str, i, len);
            if (flags & QTextEngine::RightToLeft)
                codePoints[i] = QChar::mirroredChar(codePoints[i]);
        }

        QVarLengthArray<UINT16> glyphIndices(len);
        HRESULT hr = m_directWriteFontFace->GetGlyphIndicesW(codePoints.data(),
                                                             len,
                                                             glyphIndices.data());

        if (SUCCEEDED(hr)) {
            for (int i=0; i<len; ++i)
                glyphs->glyphs[i] = glyphIndices[i];

            *nglyphs = len;

            if (!(flags & QTextEngine::GlyphIndicesOnly))
                recalcAdvances(glyphs, 0);

            return true;
        } else {
            qErrnoWarning("QFontEngineDirectWrite::stringToCMap: GetGlyphIndicesW failed");
        }
    }

    return false;
}

void QFontEngineDirectWrite::recalcAdvances(QGlyphLayout *glyphs, QTextEngine::ShaperFlags) const
{
    if (m_directWriteFontFace == 0)
        return;

    QVarLengthArray<UINT16> glyphIndices(glyphs->numGlyphs);

    // ### Caching?
    for(int i=0; i<glyphs->numGlyphs; i++)
        glyphIndices[i] = UINT16(glyphs->glyphs[i]);

    QVarLengthArray<DWRITE_GLYPH_METRICS> glyphMetrics(glyphIndices.size());
    HRESULT hr = m_directWriteFontFace->GetDesignGlyphMetrics(glyphIndices.data(),
                                                              glyphIndices.size(),
                                                              glyphMetrics.data());
    if (SUCCEEDED(hr)) {
        for (int i=0; i<glyphs->numGlyphs; ++i) {
            glyphs->advances_x[i] = DESIGN_TO_LOGICAL(glyphMetrics[i].advanceWidth);
            if (fontDef.styleStrategy & QFont::ForceIntegerMetrics)
                glyphs->advances_x[i] = glyphs->advances_x[i].round();
            glyphs->advances_y[i] = 0;
        }
    } else {
        qErrnoWarning("QFontEngineDirectWrite::recalcAdvances: GetDesignGlyphMetrics failed");
    }
}

void QFontEngineDirectWrite::addGlyphsToPath(glyph_t *glyphs, QFixedPoint *positions, int nglyphs,
                                             QPainterPath *path, QTextItem::RenderFlags flags)
{
    if (m_directWriteFontFace == 0)
        return;

    QVarLengthArray<UINT16> glyphIndices(nglyphs);
    QVarLengthArray<DWRITE_GLYPH_OFFSET> glyphOffsets(nglyphs);
    QVarLengthArray<FLOAT> glyphAdvances(nglyphs);

    for (int i=0; i<nglyphs; ++i) {
        glyphIndices[i] = glyphs[i];
        glyphOffsets[i].advanceOffset  = positions[i].x.toReal();
        glyphOffsets[i].ascenderOffset = -positions[i].y.toReal();
        glyphAdvances[i] = 0.0;
    }

    GeometrySink geometrySink(path);
    HRESULT hr = m_directWriteFontFace->GetGlyphRunOutline(
                fontDef.pixelSize,
                glyphIndices.data(),
                glyphAdvances.data(),
                glyphOffsets.data(),
                nglyphs,
                false,
                flags & QTextItem::RightToLeft,
                &geometrySink
                );

    if (FAILED(hr))
        qErrnoWarning("QFontEngineDirectWrite::addGlyphsToPath: GetGlyphRunOutline failed");
}

glyph_metrics_t QFontEngineDirectWrite::boundingBox(const QGlyphLayout &glyphs)
{
    if (glyphs.numGlyphs == 0)
        return glyph_metrics_t();

    bool round = fontDef.styleStrategy & QFont::ForceIntegerMetrics;

    QFixed w = 0;
    for (int i = 0; i < glyphs.numGlyphs; ++i) {
        w += round ? glyphs.effectiveAdvance(i).round() : glyphs.effectiveAdvance(i);

    }

    return glyph_metrics_t(0, -m_ascent, w - lastRightBearing(glyphs), m_ascent + m_descent, w, 0);
}

glyph_metrics_t QFontEngineDirectWrite::boundingBox(glyph_t g)
{
    if (m_directWriteFontFace == 0)
        return glyph_metrics_t();

    UINT16 glyphIndex = g;

    DWRITE_GLYPH_METRICS glyphMetrics;
    HRESULT hr = m_directWriteFontFace->GetDesignGlyphMetrics(&glyphIndex, 1, &glyphMetrics);
    if (SUCCEEDED(hr)) {
        QFixed advanceWidth = DESIGN_TO_LOGICAL(glyphMetrics.advanceWidth);
        QFixed leftSideBearing = DESIGN_TO_LOGICAL(glyphMetrics.leftSideBearing);
        QFixed rightSideBearing = DESIGN_TO_LOGICAL(glyphMetrics.rightSideBearing);
        QFixed advanceHeight = DESIGN_TO_LOGICAL(glyphMetrics.advanceHeight);
        QFixed verticalOriginY = DESIGN_TO_LOGICAL(glyphMetrics.verticalOriginY);

        if (fontDef.styleStrategy & QFont::ForceIntegerMetrics) {
            advanceWidth = advanceWidth.round();
            advanceHeight = advanceHeight.round();
        }

        QFixed width = advanceWidth - leftSideBearing - rightSideBearing;

        return glyph_metrics_t(-leftSideBearing, -verticalOriginY,
                               width, m_ascent + m_descent,
                               advanceWidth, advanceHeight);
    } else {
        qErrnoWarning("QFontEngineDirectWrite::boundingBox: GetDesignGlyphMetrics failed");
    }

    return glyph_metrics_t();
}

QFixed QFontEngineDirectWrite::ascent() const
{
    return fontDef.styleStrategy & QFont::ForceIntegerMetrics
            ? m_ascent.round()
            : m_ascent;
}

QFixed QFontEngineDirectWrite::descent() const
{
    return fontDef.styleStrategy & QFont::ForceIntegerMetrics
           ? (m_descent - 1).round()
           : (m_descent - 1);
}

QFixed QFontEngineDirectWrite::leading() const
{
    return fontDef.styleStrategy & QFont::ForceIntegerMetrics
           ? m_lineGap.round()
           : m_lineGap;
}

QFixed QFontEngineDirectWrite::xHeight() const
{
    return fontDef.styleStrategy & QFont::ForceIntegerMetrics
           ? m_xHeight.round()
           : m_xHeight;
}

qreal QFontEngineDirectWrite::maxCharWidth() const
{
    // ###
    return 0;
}

extern uint qt_pow_gamma[256];

QImage QFontEngineDirectWrite::alphaMapForGlyph(glyph_t glyph, QFixed subPixelPosition)
{
    QImage im = imageForGlyph(glyph, subPixelPosition, 0, QTransform());

    QImage indexed(im.width(), im.height(), QImage::Format_Indexed8);
    QVector<QRgb> colors(256);
    for (int i=0; i<256; ++i)
        colors[i] = qRgba(0, 0, 0, i);
    indexed.setColorTable(colors);

    for (int y=0; y<im.height(); ++y) {
        uint *src = (uint*) im.scanLine(y);
        uchar *dst = indexed.scanLine(y);
        for (int x=0; x<im.width(); ++x) {
            *dst = 255 - (qt_pow_gamma[qGray(0xffffffff - *src)] * 255. / 2047.);
            ++dst;
            ++src;
        }
    }

    return indexed;
}

bool QFontEngineDirectWrite::supportsSubPixelPositions() const
{
    return true;
}

QImage QFontEngineDirectWrite::imageForGlyph(glyph_t t,
                                             QFixed subPixelPosition,
                                             int margin,
                                             const QTransform &xform)
{
    glyph_metrics_t metrics = QFontEngine::boundingBox(t, xform);
    int width = (metrics.width + margin * 2 + 4).ceil().toInt() ;
    int height = (metrics.height + margin * 2 + 4).ceil().toInt();

    UINT16 glyphIndex = t;
    FLOAT glyphAdvance = metrics.xoff.toReal();

    DWRITE_GLYPH_OFFSET glyphOffset;
    glyphOffset.advanceOffset = 0;
    glyphOffset.ascenderOffset = 0;

    DWRITE_GLYPH_RUN glyphRun;
    glyphRun.fontFace = m_directWriteFontFace;
    glyphRun.fontEmSize = fontDef.pixelSize;
    glyphRun.glyphCount = 1;
    glyphRun.glyphIndices = &glyphIndex;
    glyphRun.glyphAdvances = &glyphAdvance;
    glyphRun.isSideways = false;
    glyphRun.bidiLevel = 0;
    glyphRun.glyphOffsets = &glyphOffset;

    QFixed x = margin - metrics.x.round() + subPixelPosition;
    QFixed y = margin - metrics.y.floor();

    DWRITE_MATRIX transform;
    transform.dx = x.toReal();
    transform.dy = y.toReal();
    transform.m11 = xform.m11();
    transform.m12 = xform.m12();
    transform.m21 = xform.m21();
    transform.m22 = xform.m22();

    IDWriteGlyphRunAnalysis *glyphAnalysis = NULL;
    HRESULT hr = m_directWriteFactory->CreateGlyphRunAnalysis(
                &glyphRun,
                1.0f,
                &transform,
                DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC,
                DWRITE_MEASURING_MODE_NATURAL,
                0.0, 0.0,
                &glyphAnalysis
                );

    if (SUCCEEDED(hr)) {
        RECT rect;
        rect.left = 0;
        rect.top = 0;
        rect.right = width;
        rect.bottom = height;

        int size = width * height * 3;
        BYTE *alphaValues = new BYTE[size];
        qMemSet(alphaValues, size, 0);

        hr = glyphAnalysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1,
                                               &rect,
                                               alphaValues,
                                               size);

        if (SUCCEEDED(hr)) {
            QImage img(width, height, QImage::Format_RGB32);
            img.fill(0xffffffff);

            for (int y=0; y<height; ++y) {
                uint *dest = reinterpret_cast<uint *>(img.scanLine(y));
                BYTE *src = alphaValues + width * 3 * y;

                for (int x=0; x<width; ++x) {
                    dest[x] = *(src) << 16
                            | *(src + 1) << 8
                            | *(src + 2);

                    src += 3;
                }
            }

            delete[] alphaValues;
            glyphAnalysis->Release();

            return img;
        } else {
            delete[] alphaValues;
            glyphAnalysis->Release();

            qErrnoWarning("QFontEngineDirectWrite::imageForGlyph: CreateAlphaTexture failed");
        }

    } else {
        qErrnoWarning("QFontEngineDirectWrite::imageForGlyph: CreateGlyphRunAnalysis failed");
    }

    return QImage();
}

QImage QFontEngineDirectWrite::alphaRGBMapForGlyph(glyph_t t,
                                                   QFixed subPixelPosition,
                                                   int margin,
                                                   const QTransform &xform)
{
    QImage mask = imageForGlyph(t, subPixelPosition, margin, xform);
    return mask.depth() == 32
           ? mask
           : mask.convertToFormat(QImage::Format_RGB32);
}

const char *QFontEngineDirectWrite::name() const
{
    return 0;
}

bool QFontEngineDirectWrite::canRender(const QChar *string, int len)
{
    for (int i=0; i<len; ++i) {
        BOOL exists;
        UINT32 codePoint = getChar(string, i, len);
        HRESULT hr = m_directWriteFont->HasCharacter(codePoint, &exists);
        if (FAILED(hr)) {
            qErrnoWarning("QFontEngineDirectWrite::canRender: HasCharacter failed");
            return false;
        } else if (!exists) {
            return false;
        }
    }

    return true;
}

QFontEngine::Type QFontEngineDirectWrite::type() const
{
    return QFontEngine::DirectWrite;
}

QT_END_NAMESPACE

#endif // QT_NO_DIRECTWRITE