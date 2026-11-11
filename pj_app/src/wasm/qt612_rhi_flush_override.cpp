// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
//
// Link-time patch of two Qt 6.12.0 functions, WebAssembly build only (PJ_WASM_QT_FLUSH_OVERRIDE).
//
// Problem: in a window that contains QRhiWidgets, any raster repaint (the playback time bar, every
// tick) made Qt add the full rect of every dirty QRhiWidget to the raster damage and upload the
// bounding box of it all: close to the whole window, ~35 MB per frame at devicePixelRatio 2.
//
//  A) QWidgetRepaintManager::paintAndFlush, from qtbase v6.12.0
//     src/widgets/kernel/qwidgetrepaintmanager.cpp: a dirty QRhiWidget that does not touch the
//     raster damage is painted directly instead of joining it. Changed only between
//     "PJ PATCH" markers. Not upstream yet; identical on qtbase's 6.12.1 branch.
//  B) QBackingStoreDefaultCompositor::toTexture(const QImage &, ...), verbatim from qtbase's
//     6.12.1 branch: backport of upstream commit 78b521d6 (QTBUG-151881), which copies and
//     converts only the dirty part of the backing store instead of the whole image.
//
// The file-static helpers are verbatim from qwidgetrepaintmanager.cpp. Kept in Qt's formatting so
// each part diffs cleanly against qtbase. Linked with --allow-multiple-definition: these
// definitions come first on the link line and win over the static Qt archives' (verified for
// the aqt 6.12.0 wasm_multithread kit: Qt reaches both functions through symbol relocations).
//
// On a Qt bump: from 6.12.1 delete B (it ships in Qt). Delete A and this file once Qt carries an
// equivalent fix; until then re-derive A from the new qwidgetrepaintmanager.cpp.

// clang-format off

#include <QtCore/qglobal.h>

#if QT_VERSION != QT_VERSION_CHECK(6, 12, 0)
#error "qt612_rhi_flush_override.cpp patches Qt 6.12.0 internals; re-derive it for this Qt version (see the header)"
#endif

#include <QtCore/qdebug.h>
#include <QtCore/qvarlengtharray.h>
#include <QtGui/private/qbackingstoredefaultcompositor_p.h>
#include <QtGui/private/qguiapplication_p.h>
#include <QtGui/private/qwindow_p.h>
#include <QtGui/qpaintengine.h>
#include <QtWidgets/private/qwidget_p.h>
#include <QtWidgets/private/qwidgetrepaintmanager_p.h>
#include <QtWidgets/qapplication.h>
#if QT_CONFIG(graphicsview)
#include <QtWidgets/qgraphicsproxywidget.h>
#endif
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
        // Stop at native widgets but store them. Stop at hidden widgets too.
        if (w && !w->isWindow() && hasPlatformWindow(w))
            nativeChildren->append(w);
        if (w && !w->isWindow() && !hasPlatformWindow(w) && !w->isHidden() && QWidgetPrivate::get(w)->textureChildSeen)
            findTextureWidgetsRecursively(tlw, w, widgetTextures, nativeChildren);
    }
}

static void findAllTextureWidgetsRecursively(QWidget *tlw, QWidget *widget)
{
    // textureChildSeen does not take native child widgets into account and that's good.
    if (QWidgetPrivate::get(widget)->textureChildSeen) {
        QList<QWidget *> nativeChildren;
        auto tl = std::make_unique<QPlatformTextureList>();
        // Look for texture widgets (incl. widget itself) from 'widget' down,
        // but skip subtrees with a parent of a native child widget.
        findTextureWidgetsRecursively(tlw, widget, tl.get(), &nativeChildren);
        // tl may be empty regardless of textureChildSeen if we have native or hidden children.
        if (!tl->isEmpty())
            QWidgetPrivate::get(tlw)->topData()->widgetTextures.push_back(std::move(tl));
        // Native child widgets, if there was any, get their own separate QPlatformTextureList.
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
    // PJ PATCH: Qt's qCInfo(lcWidgetPainting) line removed; that category is only reachable
    // from inside Qt's own build (QtPrivateLogging).

    // PJ PATCH begin: announce once that this copy replaced Qt's, which it would silently stop
    // doing if a future Qt inlined paintAndFlush into its callers.
    static bool pjPatchAnnounced = false;
    if (!pjPatchAnnounced) {
        pjPatchAnnounced = true;
        qInfo("PlotJuggler: Qt 6.12.0 paintAndFlush/toTexture link-time patch active");
    }
    // PJ PATCH end

    const bool updatesDisabled = !tlw->updatesEnabled();
    bool repaintAllWidgets = false;

    const QRect tlwRect = tlw->data->crect;
    if (!updatesDisabled && store->size() != tlwRect.size()) {
        QPlatformIntegration *integration = QGuiApplicationPrivate::platformIntegration();
        if (hasStaticContents() && !store->size().isEmpty()
            && integration->hasCapability(QPlatformIntegration::BackingStoreStaticContents)) {
            // Repaint existing dirty area and newly visible area.
            const QRect clipRect(QPoint(0, 0), store->size());
            const QRegion staticRegion(staticContents(nullptr, clipRect));
            QRegion newVisible(0, 0, tlwRect.width(), tlwRect.height());
            newVisible -= staticRegion;
            dirty += newVisible;
            store->setStaticContents(staticRegion);
        } else {
            // Repaint everything.
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

    // Contains everything that needs repaint.
    QRegion toClean(dirty);

    // Loop through all update() widgets and remove them from the list before they are
    // painted (in case someone calls update() in paintEvent). If the widget is opaque
    // and does not have transparent overlapping siblings, append it to the
    // opaqueNonOverlappedWidgets list and paint it directly without composition.
    QVarLengthArray<QWidget *, 32> opaqueNonOverlappedWidgets;
    for (int i = 0; i < dirtyWidgets.size(); ++i) {
        QWidget *w = dirtyWidgets.at(i);
        QWidgetPrivate *wd = w->d_func();
        if (wd->data.in_destructor)
            continue;

        // Clip with mask() and clipRect().
        wd->dirty &= wd->clipRect();
        wd->clipToEffectiveMask(wd->dirty);

        // Subtract opaque siblings and children.
        bool hasDirtySiblingsAbove = false;
        // We know for sure that the widget isn't overlapped if 'isMoved' is true.
        if (!wd->isMoved)
            wd->subtractOpaqueSiblings(wd->dirty, &hasDirtySiblingsAbove);

        // Make a copy of the widget's dirty region, to restore it in case there is an opaque
        // render-to-texture child that completely covers the widget, because otherwise the
        // render-to-texture child won't be visible, due to its parent widget not being redrawn
        // with a proper blending mask.
        const QRegion dirtyBeforeSubtractedOpaqueChildren = wd->dirty;

        // Scrolled and moved widgets must draw all children.
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

    // Find all render-to-texture child widgets (including self).
    // The search is cut at native widget boundaries, meaning that each native child widget
    // has its own list for the subtree below it.
    QTLWExtra *tlwExtra = tlw->d_func()->topData();
    tlwExtra->widgetTextures.clear();
    findAllTextureWidgetsRecursively(tlw, tlw);

    if (toClean.isEmpty()) {
        // Nothing to repaint. However renderToTexture widgets are handled
        // specially, they are not in the regular dirty list, in order to
        // prevent triggering unnecessary backingstore painting when only the
        // texture content changes. Check if we have such widgets in the special
        // dirty list.
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

        // We might have newly exposed areas on the screen if this function was
        // called from sync(QWidget *, QRegion)), so we have to make sure those
        // are flushed. We also need to composite the renderToTexture widgets.
        flush();

        return;
    }

    // PJ PATCH (A) begin: a dirty texture widget whose rect does not touch the raster damage is
    // painted directly after the raster pass, as in the texture-only branch above, instead of
    // having its whole rect added to the raster dirty region and uploaded with the backing store.
    // The damage is snapshotted first so the decision does not depend on list order. Not done
    // inside a QGraphicsProxyWidget, whose branch below returns before painting anything.
    const QRegion rasterDamage(toClean);
    bool directPaintAllowed = true;
#if QT_CONFIG(graphicsview)
    directPaintAllowed = !tlw->d_func()->extra->proxyWidget;
#endif
    QVarLengthArray<QWidget *, 16> directPaint;
    // PJ PATCH (A) end
    for (const auto &tl : tlwExtra->widgetTextures) {
        for (int i = 0; i < tl->count(); ++i) {
            QWidget *w = static_cast<QWidget *>(tl->source(i));
            if (dirtyRenderToTextureWidgets.contains(w)) {
                const QRect rect = tl->geometry(i); // mapped to the tlw already
                // PJ PATCH (A) begin
                if (directPaintAllowed && !rasterDamage.intersects(rect)) {
                    directPaint << w;
                    continue;
                }
                // PJ PATCH (A) end
                // Set a flag to indicate that the paint event for this
                // render-to-texture widget must not to be optimized away.
                w->d_func()->renderToTextureReallyDirty = 1;
                dirty += rect;
                toClean += rect;
            }
        }
    }
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

    // Must do this before sending any paint events because
    // the size may change in the paint event.
    updateStaticContentsSize();
    const QRegion dirtyCopy(dirty);
    dirty = QRegion();
    updateRequestSent = false;

    // Paint opaque non overlapped widgets.
    for (int i = 0; i < opaqueNonOverlappedWidgets.size(); ++i) {
        QWidget *w = opaqueNonOverlappedWidgets[i];
        QWidgetPrivate *wd = w->d_func();

        QWidgetPrivate::DrawWidgetFlags flags = QWidgetPrivate::DrawRecursive;
        // Scrolled and moved widgets must draw all children.
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

    // Paint the rest with composition.
    if (repaintAllWidgets || !dirtyCopy.isEmpty()) {
        QWidgetPrivate::DrawWidgetFlags flags = QWidgetPrivate::DrawAsRoot | QWidgetPrivate::DrawRecursive
                | QWidgetPrivate::UseEffectRegionBounds;
        tlw->d_func()->drawWidget(store->paintDevice(), dirtyCopy, QPoint(), flags, nullptr, this);
    }

    store->endPaint();

    // PJ PATCH (A) begin: paint the direct texture widgets only now that every raster widget has
    // been reset, so an update() they trigger on a raster widget queues a new repaint instead of
    // being dropped by a widget still marked as in the dirty list.
    bool directPaintedInTlw = false;
    for (QWidget *w : std::as_const(directPaint)) {
        w->d_func()->sendPaintEvent(w->rect());
        if (w != tlw) {
            QWidget *npw = w->nativeParentWidget();
            if (hasPlatformWindow(w) || (npw && npw != tlw)) {
                if (!hasPlatformWindow(w))
                    w = npw;
                markNeedsFlush(w);
                continue;
            }
        }
        directPaintedInTlw = true;
    }
    // flush() composites the top level's textures only when the top level needs a flush or
    // nothing does. If only native children do, flush the top level too (empty region: no upload).
    if (directPaintedInTlw && topLevelNeedsFlush.isEmpty() && !needsFlushWidgets.isEmpty())
        markNeedsFlush(tlw, QRegion());
    // PJ PATCH (A) end

    flush();
}

// ---- B: QBackingStoreDefaultCompositor::toTexture(const QImage &, ...), qtbase 6.12.1 ----

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
        // no fast path atm
        needsConversion = true;
        break;
    case QImage::Format_RGB30:
    case QImage::Format_A2RGB30_Premultiplied:
        // no fast path atm
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

    if (resized) {
        if (needsConversion)
            image = image.convertToFormat(QImage::Format_RGBA8888);
        else
            image.detach(); // if it was just wrapping data, that's no good, we need ownership, so detach

        if (!m_texture)
            m_texture.reset(rhi->newTexture(QRhiTexture::RGBA8, image.size()));
        else
            m_texture->setPixelSize(image.size());
        if (!m_texture->create()) {
            // Typically a lost device. Make sure to reset the null native resource
            qWarning("QBackingStoreDefaultCompositor: Failed to create backing store texture");
            m_texture.reset();
            return nullptr;
        }
        resourceUpdates->uploadTexture(m_texture.get(), image);
    } else {
        const QRect rect = dirtyRegion.boundingRect() & image.rect();
        if (rect.isEmpty())
            return m_texture.get();
        // The upload is deferred, so take ownership, but only of the dirty part
        QImage dirtyImage = image.copy(rect);
        if (needsConversion)
            dirtyImage.convertTo(QImage::Format_RGBA8888);
        QRhiTextureSubresourceUploadDescription subresDesc(dirtyImage);
        subresDesc.setDestinationTopLeft(rect.topLeft());
        QRhiTextureUploadDescription uploadDesc(QRhiTextureUploadEntry(0, 0, subresDesc));
        resourceUpdates->uploadTexture(m_texture.get(), uploadDesc);
    }

    return m_texture.get();
}

QT_END_NAMESPACE
// clang-format on
