#include "wiiu.h"

#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>
#include <stdarg.h>

#include <whb/proc.h>
#include <whb/gfx.h>
#include <coreinit/fastmutex.h>
#include <gx2/mem.h>
#include <gx2/draw.h>
#include <gx2/registers.h>

#include "shaders/display.h"
#include "overlay.h"

#define ATTRIB_SIZE (8 * 2 * sizeof(float))
#define ATTRIB_STRIDE (4 * sizeof(float))

static GX2Sampler tvScreenSamp;
static GX2Sampler drcScreenSamp;
static WHBGfxShaderGroup shaderGroup;

static float* tvAttribs;
static float* drcAttribs;

static float tvScreenSize[2];
static float drcScreenSize[2];

static uint32_t currentFrame;
uint32_t nextFrame;
static uint32_t skippedFrames;
static uint32_t droppedFrames;
static uint32_t decodedFps;
static uint32_t displayedFps;
static uint32_t skippedFps;
static uint32_t droppedFps;
static uint32_t lastDecodedFrames;
static uint32_t lastDisplayedFrames;
static uint32_t lastSkippedFrames;
static uint32_t lastDroppedFrames;
static uint64_t lastFrameStatsTime;

static OSFastMutex queueMutex;
static yuv_texture_t* queueMessages[MAX_QUEUEMESSAGES];
static uint32_t queueWriteIndex;
static uint32_t queueReadIndex;
static uint32_t tvFilterMode = WIIU_STREAM_FILTER_POINT;
static uint32_t drcFilterMode = WIIU_STREAM_FILTER_LINEAR;
static bool tvFilterDirty;
static bool drcFilterDirty;

static void* get_frame(void);

static uint32_t gx2_filter_mode(uint32_t filterMode)
{
  switch (filterMode) {
    case WIIU_STREAM_FILTER_LINEAR:
      return GX2_TEX_XY_FILTER_MODE_LINEAR;
    case WIIU_STREAM_FILTER_POINT:
    default:
      return GX2_TEX_XY_FILTER_MODE_POINT;
  }
}

uint32_t wiiu_stream_get_tv_filter_mode(void)
{
  return tvFilterMode;
}

void wiiu_stream_set_tv_filter_mode(uint32_t mode)
{
  if (mode > WIIU_STREAM_TV_OFF) {
    return;
  }

  tvFilterMode = mode;
  tvFilterDirty = true;
}

uint32_t wiiu_stream_get_drc_filter_mode(void)
{
  return drcFilterMode;
}

void wiiu_stream_set_drc_filter_mode(uint32_t mode)
{
  if (mode > WIIU_STREAM_FILTER_LINEAR) {
    return;
  }

  drcFilterMode = mode;
  drcFilterDirty = true;
}

static void apply_sampler_updates(void)
{
  if (tvFilterDirty) {
    if (tvFilterMode != WIIU_STREAM_TV_OFF) {
      GX2InitSampler(&tvScreenSamp, GX2_TEX_CLAMP_MODE_WRAP, gx2_filter_mode(tvFilterMode));
    }
    tvFilterDirty = false;
  }

  if (drcFilterDirty) {
    GX2InitSampler(&drcScreenSamp, GX2_TEX_CLAMP_MODE_WRAP, gx2_filter_mode(drcFilterMode));
    drcFilterDirty = false;
  }
}

static uint32_t get_queue_depth(void)
{
  OSFastMutex_Lock(&queueMutex);
  uint32_t elements_in = queueWriteIndex - queueReadIndex;
  OSFastMutex_Unlock(&queueMutex);
  return elements_in;
}

static void update_frame_rates(uint32_t displayedFrames)
{
  uint64_t now = LiGetMillis();
  if (lastFrameStatsTime == 0) {
    lastFrameStatsTime = now;
    lastDecodedFrames = nextFrame;
    lastDisplayedFrames = displayedFrames;
    lastSkippedFrames = skippedFrames;
    lastDroppedFrames = droppedFrames;
    return;
  }

  uint64_t elapsed = now - lastFrameStatsTime;
  if (elapsed < 1000) {
    return;
  }

  decodedFps = ((nextFrame - lastDecodedFrames) * 1000) / elapsed;
  displayedFps = ((displayedFrames - lastDisplayedFrames) * 1000) / elapsed;
  skippedFps = ((skippedFrames - lastSkippedFrames) * 1000) / elapsed;
  droppedFps = ((droppedFrames - lastDroppedFrames) * 1000) / elapsed;

  lastDecodedFrames = nextFrame;
  lastDisplayedFrames = displayedFrames;
  lastSkippedFrames = skippedFrames;
  lastDroppedFrames = droppedFrames;
  lastFrameStatsTime = now;
}

void wiiu_stream_init(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate)
{
  currentFrame = nextFrame = 0;
  skippedFrames = 0;
  droppedFrames = 0;
  decodedFps = 0;
  displayedFps = 0;
  skippedFps = 0;
  droppedFps = 0;
  lastDecodedFrames = 0;
  lastDisplayedFrames = 0;
  lastSkippedFrames = 0;
  lastDroppedFrames = 0;
  lastFrameStatsTime = 0;

  OSFastMutex_Init(&queueMutex, "");
  queueReadIndex = queueWriteIndex = 0;
  Overlay_Init();
  Overlay_SetStreamConfig(width, height, fps, bitrate);

  if (!WHBGfxLoadGFDShaderGroup(&shaderGroup, 0, display_gsh)) {
    printf("Cannot load shader\n");
  }

  WHBGfxInitShaderAttribute(&shaderGroup, "in_pos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
  WHBGfxInitShaderAttribute(&shaderGroup, "in_texCoord", 0, 8, GX2_ATTRIB_FORMAT_FLOAT_32_32);

  if (!WHBGfxInitFetchShader(&shaderGroup)) {
    printf("cannot init shader\n");
  }

  GX2InitSampler(&tvScreenSamp, GX2_TEX_CLAMP_MODE_WRAP, gx2_filter_mode(tvFilterMode));
  GX2InitSampler(&drcScreenSamp, GX2_TEX_CLAMP_MODE_WRAP, gx2_filter_mode(drcFilterMode));

  GX2ColorBuffer* cb = WHBGfxGetTVColourBuffer();
  tvScreenSize[0] = 1.0f / (float) cb->surface.width;
  tvScreenSize[1] = 1.0f / (float) cb->surface.height;

  tvAttribs = memalign(GX2_VERTEX_BUFFER_ALIGNMENT, ATTRIB_SIZE);
  int i = 0;

  tvAttribs[i++] = 0.0f;                      tvAttribs[i++] = 0.0f;
  tvAttribs[i++] = 0.0f;                      tvAttribs[i++] = 0.0f;

  tvAttribs[i++] = (float) cb->surface.width; tvAttribs[i++] = 0.0f;
  tvAttribs[i++] = 1.0f;                      tvAttribs[i++] = 0.0f;

  tvAttribs[i++] = (float) cb->surface.width; tvAttribs[i++] = (float) cb->surface.height;
  tvAttribs[i++] = 1.0f;                      tvAttribs[i++] = 1.0f;

  tvAttribs[i++] = 0.0f;                      tvAttribs[i++] = (float) cb->surface.height;
  tvAttribs[i++] = 0.0f;                      tvAttribs[i++] = 1.0f;
  GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, tvAttribs, ATTRIB_SIZE);

  cb = WHBGfxGetDRCColourBuffer();
  drcScreenSize[0] = 1.0f / (float) cb->surface.width;
  drcScreenSize[1] = 1.0f / (float) cb->surface.height;

  drcAttribs = memalign(GX2_VERTEX_BUFFER_ALIGNMENT, ATTRIB_SIZE);
  i = 0;

  drcAttribs[i++] = 0.0f;                      drcAttribs[i++] = 0.0f;
  drcAttribs[i++] = 0.0f;                      drcAttribs[i++] = 0.0f;

  drcAttribs[i++] = (float) cb->surface.width; drcAttribs[i++] = 0.0f;
  drcAttribs[i++] = 1.0f;                      drcAttribs[i++] = 0.0f;

  drcAttribs[i++] = (float) cb->surface.width; drcAttribs[i++] = (float) cb->surface.height;
  drcAttribs[i++] = 1.0f;                      drcAttribs[i++] = 1.0f;

  drcAttribs[i++] = 0.0f;                      drcAttribs[i++] = (float) cb->surface.height;
  drcAttribs[i++] = 0.0f;                      drcAttribs[i++] = 1.0f;
  GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, drcAttribs, ATTRIB_SIZE);
}

void wiiu_stream_draw(void)
{
  yuv_texture_t* tex = get_frame();
  if (tex) {
    currentFrame++;
    uint32_t queuedOrRenderedFrames = nextFrame >= droppedFrames ? nextFrame - droppedFrames : 0;
    if (queuedOrRenderedFrames >= NUM_BUFFERS && currentFrame <= queuedOrRenderedFrames - NUM_BUFFERS) {
      skippedFrames++;
      // display thread is behind decoder, skip frame
    }
    else {
      uint32_t displayedFrames = currentFrame - skippedFrames;
      update_frame_rates(displayedFrames);
      Overlay_SetFrameStats(nextFrame, displayedFrames, skippedFrames, droppedFrames,
                            decodedFps, displayedFps, skippedFps, droppedFps,
                            get_queue_depth());
      Overlay_Prepare();
      apply_sampler_updates();

      WHBGfxBeginRender();

      if (tvFilterMode != WIIU_STREAM_TV_OFF) {
        // TV
        WHBGfxBeginRenderTV();
        WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);

        GX2SetPixelTexture(&tex->yTex, 0);
        GX2SetPixelTexture(&tex->uvTex, 1);
        GX2SetPixelSampler(&tvScreenSamp, 0);
        GX2SetPixelSampler(&tvScreenSamp, 1);

        GX2SetFetchShader(&shaderGroup.fetchShader);
        GX2SetVertexShader(shaderGroup.vertexShader);
        GX2SetPixelShader(shaderGroup.pixelShader);

        GX2SetVertexUniformReg(0, 2, tvScreenSize);
        GX2SetAttribBuffer(0, ATTRIB_SIZE, ATTRIB_STRIDE, tvAttribs);
        GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);
        Overlay_Draw();

        WHBGfxFinishRenderTV();
      }

      // DRC
      WHBGfxBeginRenderDRC();
      WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);

      GX2SetPixelTexture(&tex->yTex, 0);
      GX2SetPixelTexture(&tex->uvTex, 1);
      GX2SetPixelSampler(&drcScreenSamp, 0);
      GX2SetPixelSampler(&drcScreenSamp, 1);

      GX2SetFetchShader(&shaderGroup.fetchShader);
      GX2SetVertexShader(shaderGroup.vertexShader);
      GX2SetPixelShader(shaderGroup.pixelShader);

      GX2SetVertexUniformReg(0, 2, drcScreenSize);
      GX2SetAttribBuffer(0, ATTRIB_SIZE, ATTRIB_STRIDE, drcAttribs);
      GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);
      Overlay_Draw();
      
      WHBGfxFinishRenderDRC();

      WHBGfxFinishRender();
    }
  }
}

void wiiu_stream_fini(void)
{
  free(tvAttribs);
  free(drcAttribs);

  WHBGfxFreeShaderGroup(&shaderGroup);
}

static void* get_frame(void)
{
  OSFastMutex_Lock(&queueMutex);

  uint32_t elements_in = queueWriteIndex - queueReadIndex;
  if(elements_in == 0) {
    OSFastMutex_Unlock(&queueMutex);
    return NULL; // framequeue is empty
  }

  uint32_t i = (queueReadIndex)++ & (MAX_QUEUEMESSAGES - 1);
  yuv_texture_t* message = queueMessages[i];

  OSFastMutex_Unlock(&queueMutex);
  return message;
}

void add_frame(yuv_texture_t* msg)
{
  OSFastMutex_Lock(&queueMutex);

  uint32_t elements_in = queueWriteIndex - queueReadIndex;
  if (elements_in == MAX_QUEUEMESSAGES) {
    droppedFrames++;
    OSFastMutex_Unlock(&queueMutex);
    return; // framequeue is full
  }

  uint32_t i = (queueWriteIndex)++ & (MAX_QUEUEMESSAGES - 1);
  queueMessages[i] = msg;

  OSFastMutex_Unlock(&queueMutex);
}

void wiiu_setup_renderstate(void)
{
  WHBGfxBeginRenderTV();
  GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
  GX2SetBlendControl(GX2_RENDER_TARGET_0,
    GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
    GX2_BLEND_COMBINE_MODE_ADD,
    TRUE,
    GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_INV_SRC_ALPHA,
    GX2_BLEND_COMBINE_MODE_ADD
  );
  GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
  WHBGfxBeginRenderDRC();
  GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
  GX2SetBlendControl(GX2_RENDER_TARGET_0,
    GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
    GX2_BLEND_COMBINE_MODE_ADD,
    TRUE,
    GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_INV_SRC_ALPHA,
    GX2_BLEND_COMBINE_MODE_ADD
  );
  GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
}
