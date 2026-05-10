#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
  OVERLAY_INPUT_A     = 1 << 0,
  OVERLAY_INPUT_B     = 1 << 1,
  OVERLAY_INPUT_UP    = 1 << 2,
  OVERLAY_INPUT_DOWN  = 1 << 3,
  OVERLAY_INPUT_LEFT  = 1 << 4,
  OVERLAY_INPUT_RIGHT = 1 << 5,
  OVERLAY_INPUT_LT    = 1 << 6,
  OVERLAY_INPUT_RT    = 1 << 7,
  OVERLAY_INPUT_PLUS  = 1 << 8,
};

void Overlay_Init(void);
void Overlay_SetStreamConfig(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate);
void Overlay_SetFrameStats(uint32_t decodedFrames, uint32_t displayedFrames, uint32_t skippedFrames, uint32_t droppedFrames,
                           uint32_t decodedFps, uint32_t displayedFps, uint32_t skippedFps, uint32_t droppedFps,
                           uint32_t queuedFrames);
void Overlay_SetVideoStats(uint32_t frameNumber, uint32_t frameBytes, bool idrFrame, uint16_t hostProcessingLatency,
                           uint32_t receiveLatencyMs, uint32_t decoderQueueMs);
void Overlay_SetConnectionStatus(int status);
bool Overlay_InputUpdate(uint32_t held, uint32_t triggered);
void Overlay_Prepare(void);
void Overlay_Draw(void);
