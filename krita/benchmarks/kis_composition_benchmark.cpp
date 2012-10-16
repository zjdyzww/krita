/*
 *  Copyright (c) 2012 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "kis_composition_benchmark.h"

#include <qtest_kde.h>

#include <KoColorSpace.h>
#include <KoCompositeOp.h>
#include <KoColorSpaceRegistry.h>

#include <KoColorSpaceTraits.h>
#include <KoCompositeOpAlphaDarken.h>
#include <KoCompositeOpOver.h>
#include "KoOptimizedCompositeOpFactory.h"

// for calculation of the needed alignment
#include "config-vc.h"
#ifdef HAVE_VC
#include <Vc/Vc>
#include <Vc/IO>
#endif

// for memalign()
#include <malloc.h>

const int alpha_pos = 3;

void generateDataLine(uint seed, int numPixels, quint8 *srcPixels, quint8 *dstPixels, quint8 *mask)
{
    Q_ASSERT(numPixels >= 4);

    for (int i = 0; i < 4; i++) {
        srcPixels[4*i]   = i * 10 + 30;
        srcPixels[4*i+1] = i * 10 + 30;
        srcPixels[4*i+2] = i * 10 + 30;
        srcPixels[4*i+3] = i * 10 + 35;

        dstPixels[4*i]   = i * 10 + 160;
        dstPixels[4*i+1] = i * 10 + 160;
        dstPixels[4*i+2] = i * 10 + 160;
        dstPixels[4*i+3] = i * 10 + 165;

        mask[i] = i * 10 + 225;
    }

    qsrand(seed);
    numPixels -= 4;
    srcPixels += 4 * 4;
    dstPixels += 4 * 4;
    mask += 4;

    for (int i = 0; i < numPixels; i++) {
        for (int j = 0; j < 4; j++) {
            *(srcPixels++) = 1 + qrand() % 254;
            *(dstPixels++) = 1 + qrand() % 254;
        }
        *(mask++) = 1 + qrand() % 254;
    }
}

void printData(int numPixels, quint8 *srcPixels, quint8 *dstPixels, quint8 *mask)
{
    for (int i = 0; i < numPixels; i++) {
        qDebug() << "Src: "
                 << srcPixels[i*4] << "\t"
                 << srcPixels[i*4+1] << "\t"
                 << srcPixels[i*4+2] << "\t"
                 << srcPixels[i*4+3] << "\t"
                 << "Msk:" << mask[i];

        qDebug() << "Dst: "
                 << dstPixels[i*4] << "\t"
                 << dstPixels[i*4+1] << "\t"
                 << dstPixels[i*4+2] << "\t"
                 << dstPixels[i*4+3];
    }
}

const int rowStride = 64;
const int totalRows = 64;
const QRect processRect(0,0,64,64);
const int numPixels = rowStride * totalRows;
const int numTiles = 1024;


struct Tile {
    quint8 *src;
    quint8 *dst;
    quint8 *mask;
};
#include <stdint.h>
QVector<Tile> generateTiles(int size,
                            const int srcAlignmentShift,
                            const int dstAlignmentShift)
{
    QVector<Tile> tiles(size);

#ifdef HAVE_VC
    const int vecSize = Vc::float_v::Size;
#else
    const int vecSize = 1;
#endif

    for (int i = 0; i < size; i++) {
        tiles[i].src = (quint8*)memalign(vecSize * 4, numPixels * 4 + srcAlignmentShift) + srcAlignmentShift;
        tiles[i].dst = (quint8*)memalign(vecSize * 4, numPixels * 4 + dstAlignmentShift) + dstAlignmentShift;
        tiles[i].mask = (quint8*)memalign(vecSize, numPixels);

        generateDataLine(1, numPixels, tiles[i].src, tiles[i].dst, tiles[i].mask);
    }

    return tiles;
}

void freeTiles(QVector<Tile> tiles,
               const int srcAlignmentShift,
               const int dstAlignmentShift)
{
    foreach (const Tile &tile, tiles) {
        free(tile.src - srcAlignmentShift);
        free(tile.dst - dstAlignmentShift);
        free(tile.mask);
    }
}

inline bool fuzzyCompare(quint8 a, quint8 b, quint8 prec) {
    return qAbs(a - b) <= prec;
}

bool compareTwoOps(bool haveMask, const KoCompositeOp *op1, const KoCompositeOp *op2)
{
    QVector<Tile> tiles = generateTiles(2, 16, 16);

    KoCompositeOp::ParameterInfo params;
    params.dstRowStride  = 4 * rowStride;
    params.srcRowStride  = 4 * rowStride;
    params.maskRowStride = rowStride;
    params.rows          = processRect.height();
    params.cols          = processRect.width();
    params.opacity       = 0.5*1.0f;
    params.flow          = 0.3*1.0f;
    params.channelFlags  = QBitArray();

    params.dstRowStart   = tiles[0].dst;
    params.srcRowStart   = tiles[0].src;
    params.maskRowStart  = haveMask ? tiles[0].mask : 0;
    op1->composite(params);

    params.dstRowStart   = tiles[1].dst;
    params.srcRowStart   = tiles[1].src;
    params.maskRowStart  = haveMask ? tiles[1].mask : 0;
    op2->composite(params);

    quint8 *dst1 = tiles[0].dst;
    quint8 *dst2 = tiles[1].dst;
    for (int i = 0; i < numPixels; i++) {
        if (!fuzzyCompare(dst1[0], dst2[0], 2) ||
            !fuzzyCompare(dst1[1], dst2[1], 2) ||
            !fuzzyCompare(dst1[2], dst2[2], 2) ||
            !fuzzyCompare(dst1[3], dst2[3], 2)) {

            qDebug() << "Wrong result:" << i;
            qDebug() << "Act: " << dst1[0] << dst1[1] << dst1[2] << dst1[3];
            qDebug() << "Exp: " << dst2[0] << dst2[1] << dst2[2] << dst2[3];

            quint8 *src1 = tiles[0].src + 4 * i;
            quint8 *src2 = tiles[1].src + 4 * i;

            qDebug() << "SrcA:" << src1[0] << src1[1] << src1[2] << src1[3];
            qDebug() << "SrcE:" << src2[0] << src2[1] << src2[2] << src2[3];

            qDebug() << "MskA:" << tiles[0].mask[i];
            qDebug() << "MskE:" << tiles[1].mask[i];

            return false;
        }
        dst1 += 4;
        dst2 += 4;
    }

    freeTiles(tiles, 16, 16);

    return true;
}

void benchmarkCompositeOp(const KoCompositeOp *op,
                          bool haveMask,
                          const int srcAlignmentShift,
                          const int dstAlignmentShift)
{
    QVector<Tile> tiles =
        generateTiles(numTiles, srcAlignmentShift, dstAlignmentShift);

//    qDebug() << "Initial values:";
//    printData(8, tiles[0].src, tiles[0].dst, tiles[0].mask);

    const int tileOffset = 4 * (processRect.y() * rowStride + processRect.x());

    KoCompositeOp::ParameterInfo params;
    params.dstRowStride  = 4 * rowStride;
    params.srcRowStride  = 4 * rowStride;
    params.maskRowStride = rowStride;
    params.rows          = processRect.height();
    params.cols          = processRect.width();
    params.opacity       = 0.5*1.0f;
    params.flow          = 0.3*1.0f;
    params.channelFlags  = QBitArray();

    QBENCHMARK_ONCE {
        foreach (const Tile &tile, tiles) {
            params.dstRowStart   = tile.dst + tileOffset;
            params.srcRowStart   = tile.src + tileOffset;
            params.maskRowStart  = haveMask ? tile.mask : 0;
            op->composite(params);
        }
    }

//    qDebug() << "Final values:";
//    printData(8, tiles[0].src, tiles[0].dst, tiles[0].mask);

    freeTiles(tiles, srcAlignmentShift, dstAlignmentShift);
}

void KisCompositionBenchmark::compareAlphaDarkenOps()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KoCompositeOp *opAct = KoOptimizedCompositeOpFactory::createAlphaDarkenOp32(cs);
    KoCompositeOp *opExp = new KoCompositeOpAlphaDarken<KoBgrU8Traits>(cs);

    QVERIFY(compareTwoOps(false, opAct, opExp));

    delete opExp;
    delete opAct;
}

void KisCompositionBenchmark::compareOverOps()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KoCompositeOp *opAct = KoOptimizedCompositeOpFactory::createOverOp32(cs);
    KoCompositeOp *opExp = new KoCompositeOpOver<KoBgrU8Traits>(cs);

    QVERIFY(compareTwoOps(true, opAct, opExp));

    delete opExp;
    delete opAct;
}

void KisCompositionBenchmark::testRgb8CompositeAlphaDarkenLegacy_Aligned()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KoCompositeOp *op = new KoCompositeOpAlphaDarken<KoBgrU8Traits>(cs);
    benchmarkCompositeOp(op, true, 0, 0);
    delete op;
}
// Unaligned versions of the Legacy version give the same results

void KisCompositionBenchmark::testRgb8CompositeAlphaDarkenOptimized_Aligned()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KoCompositeOp *op = KoOptimizedCompositeOpFactory::createAlphaDarkenOp32(cs);
    benchmarkCompositeOp(op, true, 0, 0);
    delete op;
}
void KisCompositionBenchmark::testRgb8CompositeAlphaDarkenOptimized_SrcUnaligned()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KoCompositeOp *op = KoOptimizedCompositeOpFactory::createAlphaDarkenOp32(cs);
    benchmarkCompositeOp(op, true, 8, 0);
    delete op;
}
void KisCompositionBenchmark::testRgb8CompositeAlphaDarkenOptimized_DstUnaligned()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KoCompositeOp *op = KoOptimizedCompositeOpFactory::createAlphaDarkenOp32(cs);
    benchmarkCompositeOp(op, true, 0, 8);
    delete op;
}
void KisCompositionBenchmark::testRgb8CompositeAlphaDarkenOptimized_Unaligned()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KoCompositeOp *op = KoOptimizedCompositeOpFactory::createAlphaDarkenOp32(cs);
    benchmarkCompositeOp(op, true, 4, 8);
    delete op;
}


void KisCompositionBenchmark::testRgb8CompositeAlphaDarkenLegacy_Aligned_NoMask()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KoCompositeOp *op = new KoCompositeOpAlphaDarken<KoBgrU8Traits>(cs);
    benchmarkCompositeOp(op, false, 0, 0);
    delete op;
}
void KisCompositionBenchmark::testRgb8CompositeAlphaDarkenOptimized_Aligned_NoMask()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KoCompositeOp *op = KoOptimizedCompositeOpFactory::createAlphaDarkenOp32(cs);
    benchmarkCompositeOp(op, false, 0, 0);
    delete op;
}


void KisCompositionBenchmark::testRgb8CompositeOverLegacy_Aligned()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KoCompositeOp *op = new KoCompositeOpOver<KoBgrU8Traits>(cs);
    benchmarkCompositeOp(op, true, 0, 0);
    delete op;
}

void KisCompositionBenchmark::testRgb8CompositeOverOptimized_Aligned()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KoCompositeOp *op = KoOptimizedCompositeOpFactory::createOverOp32(cs);
    benchmarkCompositeOp(op, true, 0, 0);
    delete op;
}

void KisCompositionBenchmark::testRgb8CompositeOverOptimized_Unaligned()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KoCompositeOp *op = KoOptimizedCompositeOpFactory::createOverOp32(cs);
    benchmarkCompositeOp(op, true, 4, 8);
    delete op;
}

void KisCompositionBenchmark::testRgb8CompositeOverLegacy_Aligned_NoMask()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KoCompositeOp *op = new KoCompositeOpOver<KoBgrU8Traits>(cs);
    benchmarkCompositeOp(op, false, 0, 0);
    delete op;
}

void KisCompositionBenchmark::testRgb8CompositeOverOptimized_Aligned_NoMask()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KoCompositeOp *op = KoOptimizedCompositeOpFactory::createOverOp32(cs);
    benchmarkCompositeOp(op, false, 0, 0);
    delete op;
}

void KisCompositionBenchmark::testRgb8CompositeAlphaDarkenReal_Aligned()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const KoCompositeOp *op = cs->compositeOp(COMPOSITE_ALPHA_DARKEN);
    benchmarkCompositeOp(op, true, 0, 0);
}

void KisCompositionBenchmark::testRgb8CompositeOverReal_Aligned()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const KoCompositeOp *op = cs->compositeOp(COMPOSITE_OVER);
    benchmarkCompositeOp(op, true, 0, 0);
}


QTEST_KDEMAIN(KisCompositionBenchmark, GUI)
