/*
 * Copyright (C) 2004, 2006, 2007 Apple Inc. All rights reserved.
 * Copyright (C) 2007 Alp Toker <alp@atoker.com>
 * Copyright (C) 2010 Torch Mobile (Beijing) Co. Ltd. All rights reserved.
 * Copyright (c) 2012, The Linux Foundation All rights reserved.
 * Copyright (C) 2011, 2012 Sony Ericsson Mobile Communications AB
 * Copyright (C) 2012 Sony Mobile Communications AB
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "HTMLCanvasElement.h"

#include "Attribute.h"
#include "CanvasContextAttributes.h"
#include "CanvasGradient.h"
#include "CanvasPattern.h"
#include "CanvasRenderingContext2D.h"
#include "CanvasStyle.h"
#include "Chrome.h"
#include "Document.h"
#include "ExceptionCode.h"
#include "Frame.h"
#include "GraphicsContext.h"
#include "HTMLNames.h"
#include "ImageBuffer.h"
#include "ImageData.h"
#include "MIMETypeRegistry.h"
#include "Page.h"
#include "RenderHTMLCanvas.h"
#include "RenderLayer.h"
#include "Settings.h"
#include "CanvasLayerAndroid.h"
#include "PlatformGraphicsContext.h"
#include "RenderLayer.h"
#include <math.h>
#include <stdio.h>

#if PLATFORM(ANDROID)
#include <cutils/log.h>
#include <cutils/properties.h>
#include "CanvasLayer.h"
#endif

#if USE(JSC)
#include <runtime/JSLock.h>
#endif

#if ENABLE(WEBGL)
#include "WebGLContextAttributes.h"
#include "WebGLRenderingContext.h"
#endif

namespace WebCore {

using namespace HTMLNames;

// These values come from the WhatWG spec.
static const int DefaultWidth = 300;
static const int DefaultHeight = 150;

#if PLATFORM(ANDROID)
//TODO::VA::Make this robust later (prototyping)... needs to be protected by ifdefs for android
int HTMLCanvasElement::s_recordingCanvasThreshold = 5;
#endif

// Firefox limits width/height to 32767 pixels, but slows down dramatically before it
// reaches that limit. We limit by area instead, giving us larger maximum dimensions,
// in exchange for a smaller maximum canvas size.
static const float MaxCanvasArea = 32768 * 8192; // Maximum canvas area in CSS pixels

//In Skia, we will also limit width/height to 32767.
static const float MaxSkiaDim = 32767.0F; // Maximum width/height in CSS pixels.

HTMLCanvasElement::HTMLCanvasElement(const QualifiedName& tagName, Document* document)
    : HTMLElement(tagName, document)
    , m_size(DefaultWidth, DefaultHeight)
    , m_rendererIsCanvas(false)
    , m_ignoreReset(false)
#ifdef ANDROID
    /* In Android we capture the drawing into a displayList, and then
       replay that list at various scale factors (sometimes zoomed out, other
       times zoomed in for "normal" reading, yet other times at arbitrary
       zoom values based on the user's choice). In all of these cases, we do
       not re-record the displayList, hence it is usually harmful to perform
       any pre-rounding, since we just don't know the actual drawing resolution
       at record time.
    */
    , m_pageScaleFactor(1)
#else
    , m_pageScaleFactor(document->frame() ? document->frame()->page()->chrome()->scaleFactor() : 1)
#endif
    , m_originClean(true)
    , m_hasCreatedImageBuffer(false)
#if PLATFORM(ANDROID)
    , m_recordingCanvasEnabled(true)
    , m_gpuCanvasEnabled(true)
    , m_gpuRendering(false)
    , m_supportedCompositing(true)
    , m_canUseGpuRendering(false)
#endif
{
    ASSERT(hasTagName(canvasTag));

# if PLATFORM(ANDROID)
    char pval[PROPERTY_VALUE_MAX];
    property_get("debug.recordingcanvas", pval, "1");

    m_recordingCanvasEnabled = atoi(pval) ? true : false;

    char pval2[PROPERTY_VALUE_MAX];
    property_get("debug.gpucanvas", pval2, "1");

    m_gpuCanvasEnabled = atoi(pval2) ? true : false;

    //Allow threshold value to be set per device
    char pval3[PROPERTY_VALUE_MAX];
    property_get("debug.recordingcanvas.threshold", pval3, "5");

    s_recordingCanvasThreshold = atoi(pval3);
#endif
}

PassRefPtr<HTMLCanvasElement> HTMLCanvasElement::create(Document* document)
{
    return adoptRef(new HTMLCanvasElement(canvasTag, document));
}

PassRefPtr<HTMLCanvasElement> HTMLCanvasElement::create(const QualifiedName& tagName, Document* document)
{
    return adoptRef(new HTMLCanvasElement(tagName, document));
}

HTMLCanvasElement::~HTMLCanvasElement()
{
    HashSet<CanvasObserver*>::iterator end = m_observers.end();
    for (HashSet<CanvasObserver*>::iterator it = m_observers.begin(); it != end; ++it)
        (*it)->canvasDestroyed(this);

#if PLATFORM(ANDROID)
#if ENABLE(WEBGL)
    document()->unregisterForDocumentActivationCallbacks(this);
    document()->unregisterForDocumentSuspendCallbacks(this);
#endif
#endif
}

void HTMLCanvasElement::parseMappedAttribute(Attribute* attr)
{
    const QualifiedName& attrName = attr->name();
    if (attrName == widthAttr || attrName == heightAttr)
        reset();
    HTMLElement::parseMappedAttribute(attr);
}

RenderObject* HTMLCanvasElement::createRenderer(RenderArena* arena, RenderStyle* style)
{
    Frame* frame = document()->frame();
    if (frame && frame->script()->canExecuteScripts(NotAboutToExecuteScript)) {
        m_rendererIsCanvas = true;
        return new (arena) RenderHTMLCanvas(this);
    }

    m_rendererIsCanvas = false;
    return HTMLElement::createRenderer(arena, style);
}

void HTMLCanvasElement::addObserver(CanvasObserver* observer)
{
    m_observers.add(observer);
}

void HTMLCanvasElement::removeObserver(CanvasObserver* observer)
{
    m_observers.remove(observer);
}

void HTMLCanvasElement::setHeight(int value)
{
    setAttribute(heightAttr, String::number(value));
}

void HTMLCanvasElement::setWidth(int value)
{
    setAttribute(widthAttr, String::number(value));
}

CanvasRenderingContext* HTMLCanvasElement::getContext(const String& type, CanvasContextAttributes* attrs)
{
    // A Canvas can either be "2D" or "webgl" but never both. If you request a 2D canvas and the existing
    // context is already 2D, just return that. If the existing context is WebGL, then destroy it
    // before creating a new 2D context. Vice versa when requesting a WebGL canvas. Requesting a
    // context with any other type string will destroy any existing context.

    // FIXME - The code depends on the context not going away once created, to prevent JS from
    // seeing a dangling pointer. So for now we will disallow the context from being changed
    // once it is created.
    if (type == "2d") {
        if (m_context && !m_context->is2d())
            return 0;
        if (!m_context) {
            bool usesDashbardCompatibilityMode = false;
#if ENABLE(DASHBOARD_SUPPORT)
            if (Settings* settings = document()->settings())
                usesDashbardCompatibilityMode = settings->usesDashboardBackwardCompatibilityMode();
#endif
            m_context = adoptPtr(new CanvasRenderingContext2D(this, document()->inQuirksMode(), usesDashbardCompatibilityMode));
#if USE(IOSURFACE_CANVAS_BACKING_STORE) || (ENABLE(ACCELERATED_2D_CANVAS) && USE(ACCELERATED_COMPOSITING)) || PLATFORM(ANDROID)
            if (m_context) {
                // Need to make sure a RenderLayer and compositing layer get created for the Canvas
                setNeedsStyleRecalc(SyntheticStyleChange);
            }
#endif
        }
        return m_context.get();
    }
#if ENABLE(WEBGL)
    Settings* settings = document()->settings();
    if (settings && settings->webGLEnabled()
#if !PLATFORM(CHROMIUM) && !PLATFORM(GTK)
        && settings->acceleratedCompositingEnabled()
#endif
        ) {
        // Accept the legacy "webkit-3d" name as well as the provisional "experimental-webgl" name.
        // Once ratified, we will also accept "webgl" as the context name.
        if ((type == "webkit-3d") ||
            (type == "experimental-webgl")) {
            if (m_context && !m_context->is3d())
                return 0;
            if (!m_context) {
                m_context = WebGLRenderingContext::create(this, static_cast<WebGLContextAttributes*>(attrs));
                if (m_context) {
                    // Need to make sure a RenderLayer and compositing layer get created for the Canvas
                    setNeedsStyleRecalc(SyntheticStyleChange);
#if PLATFORM(ANDROID)
                    document()->registerForDocumentActivationCallbacks(this);
                    document()->registerForDocumentSuspendCallbacks(this);
                    document()->setContainsWebGLContent(true);
#endif
                }
            }
            return m_context.get();
        }
    }
#else
    UNUSED_PARAM(attrs);
#endif
    return 0;
}

void HTMLCanvasElement::didDraw(const FloatRect& rect)
{
    m_copiedImage.clear(); // Clear our image snapshot if we have one.

    if (RenderBox* ro = renderBox()) {
        FloatRect destRect = ro->contentBoxRect();
        FloatRect r = mapRect(rect, FloatRect(0, 0, size().width(), size().height()), destRect);
        r.intersect(destRect);
        if (r.isEmpty() || m_dirtyRect.contains(r))
            return;

        m_dirtyRect.unite(r);
#if PLATFORM(ANDROID)
        // We handle invals ourselves and don't want webkit to repaint if we
        // have put the canvas on a layer
        if (!ro->hasLayer())
#endif
        ro->repaintRectangle(enclosingIntRect(m_dirtyRect));
    }

    HashSet<CanvasObserver*>::iterator end = m_observers.end();
    for (HashSet<CanvasObserver*>::iterator it = m_observers.begin(); it != end; ++it)
        (*it)->canvasChanged(this, rect);
}

void HTMLCanvasElement::reset()
{
    if (m_ignoreReset)
        return;

    bool ok;
    bool hadImageBuffer = hasCreatedImageBuffer();
    int w = getAttribute(widthAttr).toInt(&ok);
    if (!ok || w < 0)
        w = DefaultWidth;
    int h = getAttribute(heightAttr).toInt(&ok);
    if (!ok || h < 0)
        h = DefaultHeight;

    IntSize oldSize = size();
    setSurfaceSize(IntSize(w, h)); // The image buffer gets cleared here.

#if ENABLE(WEBGL)
    if (m_context && m_context->is3d() && oldSize != size())
        static_cast<WebGLRenderingContext*>(m_context.get())->reshape(width(), height());
#endif

    if (m_context && m_context->is2d())
        static_cast<CanvasRenderingContext2D*>(m_context.get())->reset();

    if (RenderObject* renderer = this->renderer()) {
        if (m_rendererIsCanvas) {
            if (oldSize != size())
                toRenderHTMLCanvas(renderer)->canvasSizeChanged();
            if (hadImageBuffer)
                renderer->repaint();
        }
    }

    HashSet<CanvasObserver*>::iterator end = m_observers.end();
    for (HashSet<CanvasObserver*>::iterator it = m_observers.begin(); it != end; ++it)
        (*it)->canvasResized(this);
}

void HTMLCanvasElement::paint(GraphicsContext* context, const IntRect& r)
{
    // Clear the dirty rect
    m_dirtyRect = FloatRect();

    if (context->paintingDisabled())
        return;

    if (m_context) {
        if (!m_context->paintsIntoCanvasBuffer())
            return;
        m_context->paintRenderingResultsToCanvas();
    }

    if (hasCreatedImageBuffer()) {
        ImageBuffer* imageBuffer = buffer();
        if (imageBuffer) {

            if(imageBuffer->drawsUsingRecording())
                return;

            if (m_presentedImage)
                context->drawImage(m_presentedImage.get(), ColorSpaceDeviceRGB, r);
            else if (imageBuffer->drawsUsingCopy())
                context->drawImage(copiedImage(), ColorSpaceDeviceRGB, r);
            else
                context->drawImageBuffer(imageBuffer, ColorSpaceDeviceRGB, r);
        }
    }

#if ENABLE(WEBGL)
    if (is3D())
        static_cast<WebGLRenderingContext*>(m_context.get())->markLayerComposited();
#endif
#if ENABLE(DASHBOARD_SUPPORT)
    Settings* settings = document()->settings();
    if (settings && settings->usesDashboardBackwardCompatibilityMode())
        setIeForbidsInsertHTML();
#endif
}

#if PLATFORM(ANDROID)
bool HTMLCanvasElement::canUseGpuRendering()
{
    return (m_supportedCompositing && m_gpuCanvasEnabled);
}
#endif

#if ENABLE(WEBGL)
bool HTMLCanvasElement::is3D() const
{
    return m_context && m_context->is3d();
}

#if PLATFORM(ANDROID)
void HTMLCanvasElement::documentDidBecomeActive()
{
    if (m_context && m_context->is3d()) {
        WebGLRenderingContext* context3D = static_cast<WebGLRenderingContext*>(m_context.get());
        context3D->recreateSurface();
    }
}

void HTMLCanvasElement::documentWillBecomeInactive()
{
    if (m_context && m_context->is3d()) {
        WebGLRenderingContext* context3D = static_cast<WebGLRenderingContext*>(m_context.get());
        context3D->releaseSurface();
    }
}

void HTMLCanvasElement::documentWasSuspended()
{
    if (m_context && m_context->is3d()) {
        WebGLRenderingContext* context3D = static_cast<WebGLRenderingContext*>(m_context.get());
        context3D->releaseSurface();
    }
}

void HTMLCanvasElement::documentWillResume()
{
    if (m_context && m_context->is3d()) {
        WebGLRenderingContext* context3D = static_cast<WebGLRenderingContext*>(m_context.get());
        context3D->recreateSurface();
    }
}
#endif
#endif

void HTMLCanvasElement::makeRenderingResultsAvailable()
{
    if (m_context)
        m_context->paintRenderingResultsToCanvas();
}

void HTMLCanvasElement::makePresentationCopy()
{
    if (!m_presentedImage) {
        // The buffer contains the last presented data, so save a copy of it.
        m_presentedImage = buffer()->copyImage();
    }
}

void HTMLCanvasElement::clearPresentationCopy()
{
    m_presentedImage.clear();
}

void HTMLCanvasElement::setSurfaceSize(const IntSize& size)
{
    m_size = size;
    m_hasCreatedImageBuffer = false;
    m_imageBuffer.clear();
    m_copiedImage.clear();
}

String HTMLCanvasElement::toDataURL(const String& mimeType, const double* quality, ExceptionCode& ec)
{
    if (!m_originClean) {
        ec = SECURITY_ERR;
        return String();
    }

    if (m_size.isEmpty() || !buffer())
        return String("data:,");

    String lowercaseMimeType = mimeType.lower();

    // FIXME: Make isSupportedImageMIMETypeForEncoding threadsafe (to allow this method to be used on a worker thread).
    if (mimeType.isNull() || !MIMETypeRegistry::isSupportedImageMIMETypeForEncoding(lowercaseMimeType))
        lowercaseMimeType = "image/png";

#if USE(CG) || (USE(SKIA) || PLATFORM(ANDROID))
    RefPtr<ImageData> imageData = getImageData();

    if (imageData)
        return ImageDataToDataURL(*imageData, lowercaseMimeType, quality);
#endif

    makeRenderingResultsAvailable();

    return buffer()->toDataURL(lowercaseMimeType, quality);
}

PassRefPtr<ImageData> HTMLCanvasElement::getImageData()
{
    if (!m_context || !m_context->is3d())
       return 0;

#if ENABLE(WEBGL)
    WebGLRenderingContext* ctx = static_cast<WebGLRenderingContext*>(m_context.get());

    return ctx->paintRenderingResultsToImageData();
#else
    return 0;
#endif
}

IntRect HTMLCanvasElement::convertLogicalToDevice(const FloatRect& logicalRect) const
{
    // Prevent under/overflow by ensuring the rect's bounds stay within integer-expressible range
    int left = clampToInteger(floorf(logicalRect.x() * m_pageScaleFactor));
    int top = clampToInteger(floorf(logicalRect.y() * m_pageScaleFactor));
    int right = clampToInteger(ceilf(logicalRect.maxX() * m_pageScaleFactor));
    int bottom = clampToInteger(ceilf(logicalRect.maxY() * m_pageScaleFactor));

    return IntRect(IntPoint(left, top), convertToValidDeviceSize(right - left, bottom - top));
}

IntSize HTMLCanvasElement::convertLogicalToDevice(const FloatSize& logicalSize) const
{
    // Prevent overflow by ensuring the rect's bounds stay within integer-expressible range
    float width = clampToInteger(ceilf(logicalSize.width() * m_pageScaleFactor));
    float height = clampToInteger(ceilf(logicalSize.height() * m_pageScaleFactor));
    return convertToValidDeviceSize(width, height);
}

IntSize HTMLCanvasElement::convertToValidDeviceSize(float width, float height) const
{
    width = ceilf(width);
    height = ceilf(height);

    if (width < 1 || height < 1 || width * height > MaxCanvasArea)
        return IntSize();

#if USE(SKIA)
    if (width > MaxSkiaDim || height > MaxSkiaDim)
        return IntSize();
#endif

    return IntSize(width, height);
}

SecurityOrigin* HTMLCanvasElement::securityOrigin() const
{
    return document()->securityOrigin();
}

CSSStyleSelector* HTMLCanvasElement::styleSelector()
{
    return document()->styleSelector();
}

void HTMLCanvasElement::createImageBuffer() const
{
    ASSERT(!m_imageBuffer);

    m_hasCreatedImageBuffer = true;

    FloatSize unscaledSize(width(), height());
    IntSize size = convertLogicalToDevice(unscaledSize);
    if (!size.width() || !size.height())
        return;

#if USE(IOSURFACE_CANVAS_BACKING_STORE)
    if (document()->settings()->canvasUsesAcceleratedDrawing())
        m_imageBuffer = ImageBuffer::create(size, ColorSpaceDeviceRGB, Accelerated);
    else
        m_imageBuffer = ImageBuffer::create(size, ColorSpaceDeviceRGB, Unaccelerated);
#else
    m_imageBuffer = ImageBuffer::create(size);
#endif
    // The convertLogicalToDevice MaxCanvasArea check should prevent common cases
    // where ImageBuffer::create() returns 0, however we could still be low on memory.
    if (!m_imageBuffer)
        return;
    m_imageBuffer->context()->scale(FloatSize(size.width() / unscaledSize.width(), size.height() / unscaledSize.height()));
    m_imageBuffer->context()->setShadowsIgnoreTransforms(true);
    m_imageBuffer->context()->setImageInterpolationQuality(DefaultInterpolationQuality);

#if USE(JSC)
    JSC::JSLock lock(JSC::SilenceAssertionsOnly);
    scriptExecutionContext()->globalData()->heap.reportExtraMemoryCost(m_imageBuffer->dataSize());
#endif
}

#if PLATFORM(ANDROID)
void HTMLCanvasElement::enableGpuRendering()
{
    if(m_gpuRendering)
        return;

    m_gpuRendering = true;
}

void HTMLCanvasElement::disableGpuRendering()
{
    if(!m_gpuRendering)
        return;

    m_gpuRendering = false;
}

void HTMLCanvasElement::clearRecording(const FloatRect& rect)
{
    FloatRect recordingRect(0, 0, width(), height());
    if(m_imageBuffer && (rect == recordingRect))
    {
        m_canUseGpuRendering = m_imageBuffer->canUseGpuRendering();
        if(m_gpuRendering)
        {
            if(m_canUseGpuRendering)
            {
                //gpu canvas path
                IntRect r(rect.x(), rect.y(), rect.width(), rect.height());
                GraphicsContext* ctx = drawingContext();
                if(ctx)
                {
                    CanvasLayer::copyRecordingToLayer(ctx, r, m_canvasId);
                }
            }
            else
            {
                disableGpuRendering();
                CanvasLayer::setGpuCanvasStatus(m_canvasId, false);
            }
        }
        else if(m_imageBuffer->drawsUsingRecording())
        {
            IntRect r(rect.x(), rect.y(), rect.width(), rect.height());
            GraphicsContext* ctx = drawingContext();
            if(ctx)
            {
                CanvasLayer::copyRecording(ctx, r, m_canvasId);
            }
        }
        m_imageBuffer->clearRecording();
    }
}
#endif

GraphicsContext* HTMLCanvasElement::drawingContext() const
{
    return buffer() ? m_imageBuffer->context() : 0;
}

ImageBuffer* HTMLCanvasElement::buffer() const
{
    if (!m_hasCreatedImageBuffer)
        createImageBuffer();
    return m_imageBuffer.get();
}

Image* HTMLCanvasElement::copiedImage() const
{
    if (!m_copiedImage && buffer()) {
        if (m_context)
            m_context->paintRenderingResultsToCanvas();
        m_copiedImage = buffer()->copyImage();
    }
    return m_copiedImage.get();
}

void HTMLCanvasElement::clearCopiedImage()
{
    m_copiedImage.clear();
}

AffineTransform HTMLCanvasElement::baseTransform() const
{
    ASSERT(m_hasCreatedImageBuffer);
    FloatSize unscaledSize(width(), height());
    IntSize size = convertLogicalToDevice(unscaledSize);
    AffineTransform transform;
    if (size.width() && size.height())
        transform.scaleNonUniform(size.width() / unscaledSize.width(), size.height() / unscaledSize.height());
    return m_imageBuffer->baseTransform() * transform;
}

}
