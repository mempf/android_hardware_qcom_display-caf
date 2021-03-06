/*
 * Copyright (C) 2010 The Android Open Source Project
 * Copyright (C) 2012-2013, The Linux Foundation All rights reserved.
 *
 * Not a Contribution, Apache license notifications and license are retained
 * for attribution purposes only.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define HWC_UTILS_DEBUG 0
#include <sys/ioctl.h>
#include <EGL/egl.h>
#include <cutils/properties.h>
#include <gralloc_priv.h>
#include <fb_priv.h>
#include <overlay.h>
#include "hwc_utils.h"
#include "hwc_mdpcomp.h"
#include "hwc_fbupdate.h"
#include "mdp_version.h"
#include "hwc_copybit.h"
#include "external.h"
#include "QService.h"
#include "comptype.h"
namespace qhwc {

// Opens Framebuffer device
static void openFramebufferDevice(hwc_context_t *ctx)
{
    hw_module_t const *module;
    if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module) == 0) {
        framebuffer_open(module, &(ctx->mFbDev));
        private_module_t* m = reinterpret_cast<private_module_t*>(
                ctx->mFbDev->common.module);
        //xres, yres may not be 32 aligned
        ctx->dpyAttr[HWC_DISPLAY_PRIMARY].stride = m->finfo.line_length /
                                                (m->info.xres/8);
        ctx->dpyAttr[HWC_DISPLAY_PRIMARY].xres = m->info.xres;
        ctx->dpyAttr[HWC_DISPLAY_PRIMARY].yres = m->info.yres;
        ctx->dpyAttr[HWC_DISPLAY_PRIMARY].xdpi = ctx->mFbDev->xdpi;
        ctx->dpyAttr[HWC_DISPLAY_PRIMARY].ydpi = ctx->mFbDev->ydpi;
        ctx->dpyAttr[HWC_DISPLAY_PRIMARY].vsync_period =
                1000000000l / ctx->mFbDev->fps;
        ctx->dpyAttr[HWC_DISPLAY_PRIMARY].fd = openFb(HWC_DISPLAY_PRIMARY);
    }
}

void initContext(hwc_context_t *ctx)
{
    openFramebufferDevice(ctx);
    overlay::Overlay::initOverlay();
    ctx->mOverlay = overlay::Overlay::getInstance();
    ctx->mQService = qService::QService::getInstance(ctx);
    ctx->mMDP.version = qdutils::MDPVersion::getInstance().getMDPVersion();
    ctx->mMDP.hasOverlay = qdutils::MDPVersion::getInstance().hasOverlay();
    ctx->mMDP.panel = qdutils::MDPVersion::getInstance().getPanelType();
    //Is created and destroyed only once for primary
    //For external it could get created and destroyed multiple times depending
    //on what external we connect to.
    ctx->mFBUpdate[HWC_DISPLAY_PRIMARY] =
        IFBUpdate::getObject(ctx->dpyAttr[HWC_DISPLAY_PRIMARY].xres,
        HWC_DISPLAY_PRIMARY);

    char value[PROPERTY_VALUE_MAX];
    // Check if the target supports copybit compostion (dyn/mdp/c2d) to
    // decide if we need to open the copybit module.
    int compositionType =
        qdutils::QCCompositionType::getInstance().getCompositionType();

    if (compositionType & (qdutils::COMPOSITION_TYPE_DYN |
                           qdutils::COMPOSITION_TYPE_MDP |
                           qdutils::COMPOSITION_TYPE_C2D)) {
            ctx->mCopyBit[HWC_DISPLAY_PRIMARY] = new CopyBit();
    }

    ctx->mExtDisplay = new ExternalDisplay(ctx);
    for (uint32_t i = 0; i < HWC_NUM_DISPLAY_TYPES; i++)
        ctx->mLayerCache[i] = new LayerCache();
    ctx->mMDPComp = MDPComp::getObject(ctx->dpyAttr[HWC_DISPLAY_PRIMARY].xres);
    MDPComp::init(ctx);

    pthread_mutex_init(&(ctx->vstate.lock), NULL);
    pthread_cond_init(&(ctx->vstate.cond), NULL);
    ctx->vstate.enable = false;
    ctx->mExtDispConfiguring = false;
    ALOGI("Initializing Qualcomm Hardware Composer");
    ALOGI("MDP version: %d", ctx->mMDP.version);
}

void closeContext(hwc_context_t *ctx)
{
    if(ctx->mOverlay) {
        delete ctx->mOverlay;
        ctx->mOverlay = NULL;
    }

    for(int i = 0; i< HWC_NUM_DISPLAY_TYPES; i++) {
        if(ctx->mCopyBit[i]) {
            delete ctx->mCopyBit[i];
            ctx->mCopyBit[i] = NULL;
        }
    }

    if(ctx->mFbDev) {
        framebuffer_close(ctx->mFbDev);
        ctx->mFbDev = NULL;
        close(ctx->dpyAttr[HWC_DISPLAY_PRIMARY].fd);
        ctx->dpyAttr[HWC_DISPLAY_PRIMARY].fd = -1;
    }

    if(ctx->mExtDisplay) {
        delete ctx->mExtDisplay;
        ctx->mExtDisplay = NULL;
    }

    for(int i = 0; i < HWC_NUM_DISPLAY_TYPES; i++) {
        if(ctx->mFBUpdate[i]) {
            delete ctx->mFBUpdate[i];
            ctx->mFBUpdate[i] = NULL;
        }
    }

    if(ctx->mMDPComp) {
        delete ctx->mMDPComp;
        ctx->mMDPComp = NULL;
    }

    pthread_mutex_destroy(&(ctx->vstate.lock));
    pthread_cond_destroy(&(ctx->vstate.cond));
}


void dumpsys_log(android::String8& buf, const char* fmt, ...)
{
    va_list varargs;
    va_start(varargs, fmt);
    buf.appendFormatV(fmt, varargs);
    va_end(varargs);
}

/* Calculates the destination position based on the action safe rectangle */
void getActionSafePosition(hwc_context_t *ctx, int dpy, uint32_t& x,
                           uint32_t& y, uint32_t& w, uint32_t& h) {

    // if external supports underscan, do nothing
    // it will be taken care in the driver
    if(ctx->mExtDisplay->isCEUnderscanSupported())
        return;

    float wRatio = 1.0;
    float hRatio = 1.0;
    float xRatio = 1.0;
    float yRatio = 1.0;

    float fbWidth = ctx->dpyAttr[dpy].xres;
    float fbHeight = ctx->dpyAttr[dpy].yres;

    float asX = 0;
    float asY = 0;
    float asW = fbWidth;
    float asH= fbHeight;
    char value[PROPERTY_VALUE_MAX];

    // Apply action safe parameters
    property_get("hw.actionsafe.width", value, "0");
    int asWidthRatio = atoi(value);
    property_get("hw.actionsafe.height", value, "0");
    int asHeightRatio = atoi(value);
    // based on the action safe ratio, get the Action safe rectangle
    asW = fbWidth * (1.0f -  asWidthRatio / 100.0f);
    asH = fbHeight * (1.0f -  asHeightRatio / 100.0f);
    asX = (fbWidth - asW) / 2;
    asY = (fbHeight - asH) / 2;

    // calculate the position ratio
    xRatio = (float)x/fbWidth;
    yRatio = (float)y/fbHeight;
    wRatio = (float)w/fbWidth;
    hRatio = (float)h/fbHeight;

    //Calculate the position...
    x = (xRatio * asW) + asX;
    y = (yRatio * asH) + asY;
    w = (wRatio * asW);
    h = (hRatio * asH);

    return;
}

static inline bool isAlphaScaled(hwc_layer_1_t const* layer) {
    int dst_w, dst_h, src_w, src_h;

    hwc_rect_t displayFrame  = layer->displayFrame;
    hwc_rect_t sourceCrop = layer->sourceCrop;

    dst_w = displayFrame.right - displayFrame.left;
    dst_h = displayFrame.bottom - displayFrame.top;

    src_w = sourceCrop.right - sourceCrop.left;
    src_h = sourceCrop.bottom - sourceCrop.top;

    if(((src_w != dst_w) || (src_h != dst_h))) {
        if(layer->blending != HWC_BLENDING_NONE)
            return true;
    }
    return false;
}

void setListStats(hwc_context_t *ctx,
        const hwc_display_contents_1_t *list, int dpy) {

    ctx->listStats[dpy].numAppLayers = list->numHwLayers - 1;
    ctx->listStats[dpy].fbLayerIndex = list->numHwLayers - 1;
    ctx->listStats[dpy].skipCount = 0;
    ctx->listStats[dpy].needsAlphaScale = false;
    ctx->listStats[dpy].yuvCount = 0;

    for (size_t i = 0; i < list->numHwLayers; i++) {
        hwc_layer_1_t const* layer = &list->hwLayers[i];
        private_handle_t *hnd = (private_handle_t *)layer->handle;

        //reset stored yuv index
        ctx->listStats[dpy].yuvIndices[i] = -1;

        if(list->hwLayers[i].compositionType == HWC_FRAMEBUFFER_TARGET) {
            continue;
        //We disregard FB being skip for now! so the else if
        } else if (isSkipLayer(&list->hwLayers[i])) {
            ctx->listStats[dpy].skipCount++;
        } else if (UNLIKELY(isYuvBuffer(hnd))) {
            int& yuvCount = ctx->listStats[dpy].yuvCount;
            ctx->listStats[dpy].yuvIndices[yuvCount] = i;
            yuvCount++;
        }

        if(!ctx->listStats[dpy].needsAlphaScale)
            ctx->listStats[dpy].needsAlphaScale = isAlphaScaled(layer);
    }
}


static inline void calc_cut(float& leftCutRatio, float& topCutRatio,
        float& rightCutRatio, float& bottomCutRatio, int orient) {
    if(orient & HAL_TRANSFORM_FLIP_H) {
        swap(leftCutRatio, rightCutRatio);
    }
    if(orient & HAL_TRANSFORM_FLIP_V) {
        swap(topCutRatio, bottomCutRatio);
    }
    if(orient & HAL_TRANSFORM_ROT_90) {
        //Anti clock swapping
        float tmpCutRatio = leftCutRatio;
        leftCutRatio = topCutRatio;
        topCutRatio = rightCutRatio;
        rightCutRatio = bottomCutRatio;
        bottomCutRatio = tmpCutRatio;
    }
}

bool isSecuring(hwc_context_t* ctx) {
    if((ctx->mMDP.version < qdutils::MDSS_V5) &&
       (ctx->mMDP.version > qdutils::MDP_V3_0) &&
        ctx->mSecuring) {
        return true;
    }
    return false;
}

bool isSecureModePolicy(int mdpVersion) {
    if (mdpVersion < qdutils::MDSS_V5)
        return true;
    else
        return false;
}

//Crops source buffer against destination and FB boundaries
void calculate_crop_rects(hwc_rect_t& crop, hwc_rect_t& dst,
        const int fbWidth, const int fbHeight, int orient) {
    int& crop_l = crop.left;
    int& crop_t = crop.top;
    int& crop_r = crop.right;
    int& crop_b = crop.bottom;
    int crop_w = crop.right - crop.left;
    int crop_h = crop.bottom - crop.top;

    int& dst_l = dst.left;
    int& dst_t = dst.top;
    int& dst_r = dst.right;
    int& dst_b = dst.bottom;
    int dst_w = abs(dst.right - dst.left);
    int dst_h = abs(dst.bottom - dst.top);

    float leftCutRatio = 0.0f, rightCutRatio = 0.0f, topCutRatio = 0.0f,
            bottomCutRatio = 0.0f;

    if(dst_l < 0) {
        leftCutRatio = (float)(0.0f - dst_l) / (float)dst_w;
        dst_l = 0;
    }
    if(dst_r > fbWidth) {
        rightCutRatio = (float)(dst_r - fbWidth) / (float)dst_w;
        dst_r = fbWidth;
    }
    if(dst_t < 0) {
        topCutRatio = (float)(0 - dst_t) / (float)dst_h;
        dst_t = 0;
    }
    if(dst_b > fbHeight) {
        bottomCutRatio = (float)(dst_b - fbHeight) / (float)dst_h;
        dst_b = fbHeight;
    }

    calc_cut(leftCutRatio, topCutRatio, rightCutRatio, bottomCutRatio, orient);
    crop_l += crop_w * leftCutRatio;
    crop_t += crop_h * topCutRatio;
    crop_r -= crop_w * rightCutRatio;
    crop_b -= crop_h * bottomCutRatio;
}

bool isExternalActive(hwc_context_t* ctx) {
    return ctx->dpyAttr[HWC_DISPLAY_EXTERNAL].isActive;
}

int hwc_sync(hwc_context_t *ctx, hwc_display_contents_1_t* list, int dpy,
                                                        int fd) {
    int ret = 0;
    struct mdp_buf_sync data;
    int acquireFd[MAX_NUM_LAYERS];
    int count = 0;
    int releaseFd = -1;
    int fbFd = -1;
    memset(&data, 0, sizeof(data));
    bool swapzero = false;
    data.flags = MDP_BUF_SYNC_FLAG_WAIT;
    data.acq_fen_fd = acquireFd;
    data.rel_fen_fd = &releaseFd;
    char property[PROPERTY_VALUE_MAX];
    if(property_get("debug.egl.swapinterval", property, "1") > 0) {
        if(atoi(property) == 0)
            swapzero = true;
    }

    //Accumulate acquireFenceFds
    for(uint32_t i = 0; i < list->numHwLayers; i++) {
        if(list->hwLayers[i].compositionType == HWC_OVERLAY &&
                        list->hwLayers[i].acquireFenceFd != -1) {
            if(UNLIKELY(swapzero))
                acquireFd[count++] = -1;
            else
                acquireFd[count++] = list->hwLayers[i].acquireFenceFd;
        }
        if(list->hwLayers[i].compositionType == HWC_FRAMEBUFFER_TARGET) {
            if(UNLIKELY(swapzero))
                acquireFd[count++] = -1;
            else if(fd != -1) {
                //set the acquireFD from fd - which is coming from c2d
                acquireFd[count++] = fd;
                // Buffer sync IOCTL should be async when using c2d fence is
                // used
                data.flags &= ~MDP_BUF_SYNC_FLAG_WAIT;
            } else if(list->hwLayers[i].acquireFenceFd != -1)
                acquireFd[count++] = list->hwLayers[i].acquireFenceFd;
        }
    }

    data.acq_fen_fd_cnt = count;
    fbFd = ctx->dpyAttr[dpy].fd;
    //Waits for acquire fences, returns a release fence
    if(LIKELY(!swapzero)) {
        uint64_t start = systemTime();
        ret = ioctl(fbFd, MSMFB_BUFFER_SYNC, &data);
        ALOGD_IF(HWC_UTILS_DEBUG, "%s: time taken for MSMFB_BUFFER_SYNC IOCTL = %d",
                            __FUNCTION__, (size_t) ns2ms(systemTime() - start));
    }
    if(ret < 0) {
        ALOGE("ioctl MSMFB_BUFFER_SYNC failed, err=%s",
                strerror(errno));
    }
    for(uint32_t i = 0; i < list->numHwLayers; i++) {
        if(list->hwLayers[i].compositionType == HWC_OVERLAY ||
           list->hwLayers[i].compositionType == HWC_FRAMEBUFFER_TARGET) {
            //Close the acquireFenceFds
            if(list->hwLayers[i].acquireFenceFd > 0) {
                close(list->hwLayers[i].acquireFenceFd);
                list->hwLayers[i].acquireFenceFd = -1;
            }
            if(fd > 0) {
                close(fd);
                fd = -1;
            }
            //Populate releaseFenceFds.
            if(UNLIKELY(swapzero))
                list->hwLayers[i].releaseFenceFd = -1;
            else
                list->hwLayers[i].releaseFenceFd = dup(releaseFd);
        }
    }
    if(UNLIKELY(swapzero)){
        list->retireFenceFd = -1;
        close(releaseFd);
    } else {
        list->retireFenceFd = releaseFd;
    }
    return ret;
}

void LayerCache::resetLayerCache(int num) {
    for(uint32_t i = 0; i < MAX_NUM_LAYERS; i++) {
        hnd[i] = NULL;
    }
    numHwLayers = num;
}

void LayerCache::updateLayerCache(hwc_display_contents_1_t* list) {

    int numFbLayers = 0;
    int numCacheableLayers = 0;

    canUseLayerCache = false;
    //Bail if geometry changed or num of layers changed
    if(list->flags & HWC_GEOMETRY_CHANGED ||
       list->numHwLayers != numHwLayers ) {
        resetLayerCache(list->numHwLayers);
        return;
    }

    for(uint32_t i = 0; i < list->numHwLayers; i++) {
        //Bail on skip layers
        if(list->hwLayers[i].flags & HWC_SKIP_LAYER) {
            resetLayerCache(list->numHwLayers);
            return;
        }

        if(list->hwLayers[i].compositionType == HWC_FRAMEBUFFER) {
            numFbLayers++;
            if(hnd[i] == NULL) {
                hnd[i] = list->hwLayers[i].handle;
            } else if (hnd[i] ==
                       list->hwLayers[i].handle) {
                numCacheableLayers++;
            } else {
                hnd[i] = NULL;
                return;
            }
        } else {
            hnd[i] = NULL;
        }
    }
    if(numFbLayers == numCacheableLayers)
        canUseLayerCache = true;

    //XXX: The marking part is separate, if MDP comp wants
    // to use it in the future. Right now getting MDP comp
    // to use this is more trouble than it is worth.
    markCachedLayersAsOverlay(list);
}

void LayerCache::markCachedLayersAsOverlay(hwc_display_contents_1_t* list) {
    //This optimization only works if ALL the layer handles
    //that were on the framebuffer didn't change.
    if(canUseLayerCache){
        for(uint32_t i = 0; i < list->numHwLayers; i++) {
            if (list->hwLayers[i].handle &&
                list->hwLayers[i].handle == hnd[i] &&
                list->hwLayers[i].compositionType != HWC_FRAMEBUFFER_TARGET)
            {
                list->hwLayers[i].compositionType = HWC_OVERLAY;
            }
        }
    }

}

};//namespace
