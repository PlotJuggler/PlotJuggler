// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
//
// EXPERIMENT (not for merge): link-time replacement of two Qt 6.12.0 functions in the
// WebAssembly build, to measure the HiDPI playback fix before deciding how to ship it.
//
// Both bodies are copied verbatim from qtbase v6.12.0 and changed only where marked "PJ PATCH":
//  A) QWidgetRepaintManager::paintAndFlush (src/widgets/kernel/qwidgetrepaintmanager.cpp:709):
//     a dirty QRhiWidget whose rect does not touch the raster damage gets its paint event sent
//     directly (as Qt already does when only texture widgets are dirty, lines 815-849) instead of
//     having its whole rect added to the raster dirty region (lines 851-863), which made every
//     playback tick upload a near-full-window backing-store texture.
//  B) QBackingStoreDefaultCompositor::toTexture (src/gui/painting/qbackingstoredefaultcompositor.cpp:45):
//     copy only the dirty bounding box instead of detaching (deep-copying) the whole backing store.
//
// Linked with -Wl,--allow-multiple-definition so these definitions (first on the link line) win
// over the ones in the static Qt archives. Kept in Qt's own formatting so it diffs against qtbase.

// clang-format off

#include <QtCore/qglobal.h>

#if QT_VERSION != QT_VERSION_CHECK(6, 12, 0)
#error "qt612_rhi_flush_override.cpp is a copy of Qt 6.12.0 internals; re-derive it for this Qt version"
#endif

#include <QtCore/qvarlengtharray.h>
#include <QtGui/qpaintengine.h>
#include <QtGui/private/qbackingstoredefaultcompositor_p.h>
#include <QtGui/private/qguiapplication_p.h>
#include <QtGui/private/qwindow_p.h>
#include <QtWidgets/private/qwidget_p.h>
#include <QtWidgets/private/qwidgetrepaintmanager_p.h>
#include <QtWidgets/qapplication.h>
#include <QtWidgets/qgraphicsproxywidget.h>
#include <qpa/qplatformbackingstore.h>
#include <qpa/qplatformintegration.h>
#include <rhi/qrhi.h>

#include <memory>

QT_BEGIN_NAMESPACE

// ---- file-static helpers, verbatim from qwidgetrepaintmanager.cpp ----

static bool hasPlatformWindow(QWidget *widget)
{
    return widget && widget->windowHandle() && widget->windowHandle()->handle();
}

static void findTextureWidgetsRecursively(QWidget *tlw, QWidget *widget,
                                          QPlatformTextureList *widgetTextures,
                                          QList<QWidget *> *nativeChildren)
{
    QWidgetPrivate *wd = QWidgetPrivate::get(widget);
    if (wd->renderToTexture) {
        QPlatformTextureList::Flags flags = wd->textureListFlags();
        const QRect rect(widget->mapTo(tlw, QPoint()), widget->size());
        QWidgetPrivate::TextureData data = wd->texture();
        widgetTextures->appendTexture(widget, data.textureLeft, data.textureRight, rect, wd->clipRect(), flags);
    }

    for (int i = 0; i < wd->children.size(); ++i) {
        QWidget *w = qobject_cast<QWidget *>(wd->children.at(i));
        if (w && !w->isWindow() && hasPlatformWindow(w))
            nativeChildren->append(w);
        if (w && !w->isWindow() && !hasPlatformWindow(w) && !w->isHidden() && QWidgetPrivate::get(w)->textureChildSeen)
            findTextureWidgetsRecursively(tlw, w, widgetTextures, nativeChildren);
    }
}

static void findAllTextureWidgetsRecursively(QWidget *tlw, QWidget *widget)
{
    if (QWidgetPrivate::get(widget)->textureChildSeen) {
        QList<QWidget *> nativeChildren;
        auto tl = std::make_unique<QPlatformTextureList>();
        findTextureWidgetsRecursively(tlw, widget, tl.get(), &nativeChildren);
        if (!tl->isEmpty())
            QWidgetPrivate::get(tlw)->topData()->widgetTextures.push_back(std::move(tl));
        for (QWidget *ncw : std::as_const(nativeChildren)) {
            if (QWidgetPrivate::get(ncw)->textureChildSeen)
                findAllTextureWidgetsRecursively(tlw, ncw);
        }
    }
}

static bool isDrawnInEffect(const QWidget *w)
{
#if QT_CONFIG(graphicseffect)
    do {
        if (w->graphicsEffect())
            return true;
        w = w->parentWidget();
    } while (w);
#endif
    return false;
}

// ---- A: QWidgetRepaintManager::paintAndFlush ----

void QWidgetRepaintManager::paintAndFlush()
{
    const bool updatesDisabled = !tlw->updatesEnabled();
    bool repaintAllWidgets = false;

    const QRect tlwRect = tlw->data->crect;
    if (!updatesDisabled && store->size() != tlwRect.size()) {
        QPlatformIntegration *integration = QGuiApplicationPrivate::platformIntegration();
        if (hasStaticContents() && !store->size().isEmpty()
            && integration->hasCapability(QPlatformIntegration::BackingStoreStaticContents)) {
            const QRect clipRect(QPoint(0, 0), store->size());
            const QRegion staticRegion(staticContents(nullptr, clipRect));
            QRegion newVisible(0, 0, tlwRect.width(), tlwRect.height());
            newVisible -= staticRegion;
            dirty += newVisible;
            store->setStaticContents(staticRegion);
        } else {
            dirty = QRegion(0, 0, tlwRect.width(), tlwRect.height());
            for (int i = 0; i < dirtyWidgets.size(); ++i)
                resetWidget(dirtyWidgets.at(i));
            dirtyWidgets.clear();
            repaintAllWidgets = true;
        }
    }

    if (store->size() != tlwRect.size())
        store->resize(tlwRect.size());

    if (updatesDisabled)
        return;

    QRegion toClean(dirty);

    QVarLengthArray<QWidget *, 32> opaqueNonOverlappedWidgets;
    for (int i = 0; i < dirtyWidgets.size(); ++i) {
        QWidget *w = dirtyWidgets.at(i);
        QWidgetPrivate *wd = w->d_func();
        if (wd->data.in_destructor)
            continue;

        wd->dirty &= wd->clipRect();
        wd->clipToEffectiveMask(wd->dirty);

        bool hasDirtySiblingsAbove = false;
        if (!wd->isMoved)
            wd->subtractOpaqueSiblings(wd->dirty, &hasDirtySiblingsAbove);

        const QRegion dirtyBeforeSubtractedOpaqueChildren = wd->dirty;

        if (!wd->isScrolled && !wd->isMoved)
            wd->subtractOpaqueChildren(wd->dirty, w->rect());

        if (wd->dirty.isEmpty() && wd->textureChildSeen)
            wd->dirty = dirtyBeforeSubtractedOpaqueChildren;

        if (wd->dirty.isEmpty()) {
            resetWidget(w);
            continue;
        }

        const QRegion widgetDirty(w != tlw ? wd->dirty.translated(w->mapTo(tlw, QPoint()))
                                           : wd->dirty);
        toClean += widgetDirty;

#if QT_CONFIG(graphicsview)
        if (tlw->d_func()->extra->proxyWidget) {
            resetWidget(w);
            continue;
        }
#endif

        if (!isDrawnInEffect(w) && !hasDirtySiblingsAbove && wd->isOpaque
                && !dirty.intersects(widgetDirty.boundingRect())) {
            opaqueNonOverlappedWidgets.append(w);
        } else {
            resetWidget(w);
            dirty += widgetDirty;
        }
    }
    dirtyWidgets.clear();

    QTLWExtra *tlwExtra = tlw->d_func()->topData();
    tlwExtra->widgetTextures.clear();
    findAllTextureWidgetsRecursively(tlw, tlw);

    if (toClean.isEmpty()) {
        QVarLengthArray<QWidget *, 16> paintPending;
        const int numPaintPending = dirtyRenderToTextureWidgets.size();
        paintPending.reserve(numPaintPending);
        for (int i = 0; i < numPaintPending; ++i) {
            QWidget *w = dirtyRenderToTextureWidgets.at(i);
            paintPending << w;
            resetWidget(w);
        }
        dirtyRenderToTextureWidgets.clear();
        for (int i = 0; i < numPaintPending; ++i) {
            QWidget *w = paintPending[i];
            w->d_func()->sendPaintEvent(w->rect());
            if (w != tlw) {
                QWidget *npw = w->nativeParentWidget();
                if (hasPlatformWindow(w) || (npw && npw != tlw)) {
                    if (!hasPlatformWindow(w))
                        w = npw;
                    markNeedsFlush(w);
                }
            }
        }

        flush();

        return;
    }

    // PJ PATCH (A): only texture widgets that overlap the raster damage go through the raster
    // path (their rect added to dirty/toClean, so drawWidget re-punches their hole under the
    // repainted siblings). The others are painted directly below, exactly like the
    // texture-only branch above, and contribute nothing to the backing-store upload.
    // The raster damage is snapshotted first so the decision does not depend on list order.
    const QRegion rasterDamage(toClean);
    QVarLengthArray<QWidget *, 16> directPaint;
    for (const auto &tl : tlwExtra->widgetTextures) {
        for (int i = 0; i < tl->count(); ++i) {
            QWidget *w = static_cast<QWidget *>(tl->source(i));
            if (dirtyRenderToTextureWidgets.contains(w)) {
                const QRect rect = tl->geometry(i); // mapped to the tlw already
                if (!rasterDamage.intersects(rect)) {
                    directPaint << w;
                    continue;
                }
                w->d_func()->renderToTextureReallyDirty = 1;
                dirty += rect;
                toClean += rect;
            }
        }
    }
    // Snapshot-then-clear before any paint callback runs (an update() from render() must survive).
    for (int i = 0; i < dirtyRenderToTextureWidgets.size(); ++i)
        resetWidget(dirtyRenderToTextureWidgets.at(i));
    dirtyRenderToTextureWidgets.clear();

#if QT_CONFIG(graphicsview)
    if (tlw->d_func()->extra->proxyWidget) {
        updateStaticContentsSize();
        dirty = QRegion();
        updateRequestSent = false;
        for (const QRect &rect : toClean)
            tlw->d_func()->extra->proxyWidget->update(rect);
        return;
    }
#endif

    store->beginPaint(toClean);

    updateStaticContentsSize();
    const QRegion dirtyCopy(dirty);
    dirty = QRegion();
    updateRequestSent = false;

    // PJ PATCH (A): render the non-overlapping texture widgets now, before the raster painting
    // and the single flush that composites everything.
    for (QWidget *w : std::as_const(directPaint)) {
        w->d_func()->sendPaintEvent(w->rect());
        if (w != tlw) {
            QWidget *npw = w->nativeParentWidget();
            if (hasPlatformWindow(w) || (npw && npw != tlw)) {
                if (!hasPlatformWindow(w))
                    w = npw;
                markNeedsFlush(w);
            }
        }
    }

    for (int i = 0; i < opaqueNonOverlappedWidgets.size(); ++i) {
        QWidget *w = opaqueNonOverlappedWidgets[i];
        QWidgetPrivate *wd = w->d_func();

        QWidgetPrivate::DrawWidgetFlags flags = QWidgetPrivate::DrawRecursive;
        if (!wd->isScrolled && !wd->isMoved)
            flags |= QWidgetPrivate::DontDrawOpaqueChildren;
        if (w == tlw)
            flags |= QWidgetPrivate::DrawAsRoot;

        QRegion toBePainted(wd->dirty);
        resetWidget(w);

        QPoint offset;
        if (w != tlw)
            offset += w->mapTo(tlw, QPoint());
        wd->drawWidget(store->paintDevice(), toBePainted, offset, flags, nullptr, this);
    }

    if (repaintAllWidgets || !dirtyCopy.isEmpty()) {
        QWidgetPrivate::DrawWidgetFlags flags = QWidgetPrivate::DrawAsRoot | QWidgetPrivate::DrawRecursive
                | QWidgetPrivate::UseEffectRegionBounds;
        tlw->d_func()->drawWidget(store->paintDevice(), dirtyCopy, QPoint(), flags, nullptr, this);
    }

    store->endPaint();

    flush();
}

// ---- B: QBackingStoreDefaultCompositor::toTexture(const QImage &, ...) ----

QRhiTexture *QBackingStoreDefaultCompositor::toTexture(const QImage &sourceImage,
                                                       QRhi *rhi,
                                                       QRhiResourceUpdateBatch *resourceUpdates,
                                                       const QRegion &dirtyRegion,
                                                       QPlatformBackingStore::TextureFlags *flags) const
{
    Q_ASSERT(rhi);
    Q_ASSERT(resourceUpdates);
    Q_ASSERT(flags);

    if (!m_rhi) {
        m_rhi = rhi;
    } else if (m_rhi != rhi) {
        qWarning("QBackingStoreDefaultCompositor: the QRhi has changed unexpectedly, this should not happen");
        return nullptr;
    }

    QImage image = sourceImage;

    bool needsConversion = false;
    *flags = {};

    switch (image.format()) {
    case QImage::Format_ARGB32_Premultiplied:
        *flags |= QPlatformBackingStore::TexturePremultiplied;
        Q_FALLTHROUGH();
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32:
        *flags |= QPlatformBackingStore::TextureSwizzle;
        break;
    case QImage::Format_RGBA8888_Premultiplied:
        *flags |= QPlatformBackingStore::TexturePremultiplied;
        Q_FALLTHROUGH();
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA8888:
        break;
    case QImage::Format_BGR30:
    case QImage::Format_A2BGR30_Premultiplied:
        needsConversion = true;
        break;
    case QImage::Format_RGB30:
    case QImage::Format_A2RGB30_Premultiplied:
        needsConversion = true;
        break;
    default:
        needsConversion = true;
        break;
    }

    if (image.size().isEmpty())
        return nullptr;

    const bool resized = !m_texture || m_texture->pixelSize() != image.size();
    if (dirtyRegion.isEmpty() && !resized)
        return m_texture.get();

    if (needsConversion)
        image = image.convertToFormat(QImage::Format_RGBA8888);

    if (resized) {
        if (!needsConversion)
            image.detach(); // full upload: we need ownership of the whole image
        if (!m_texture)
            m_texture.reset(rhi->newTexture(QRhiTexture::RGBA8, image.size()));
        else
            m_texture->setPixelSize(image.size());
        if (!m_texture->create()) {
            qWarning("QBackingStoreDefaultCompositor: Failed to create backing store texture");
            m_texture.reset();
            return nullptr;
        }
        resourceUpdates->uploadTexture(m_texture.get(), image);
    } else {
        // PJ PATCH (B): own only the dirty bounding box instead of detaching the whole image.
        const QRect rect = dirtyRegion.boundingRect() & image.rect();
        if (rect.isEmpty())
            return m_texture.get();
        QRhiTextureSubresourceUploadDescription subresDesc(image.copy(rect));
        subresDesc.setDestinationTopLeft(rect.topLeft());
        QRhiTextureUploadDescription uploadDesc(QRhiTextureUploadEntry(0, 0, subresDesc));
        resourceUpdates->uploadTexture(m_texture.get(), uploadDesc);
    }

    return m_texture.get();
}

QT_END_NAMESPACE
// clang-format on
