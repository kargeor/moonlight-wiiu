#include "overlay.h"
#include "font.h"
#include "wiiu.h"

#include <Limelight.h>

#include <stdio.h>

typedef void (*OverlayValueFn)(char* buffer, size_t size);
typedef void (*OverlayActionFn)(void);
typedef void (*OverlayAdjustFn)(int delta);

typedef struct {
  const char* label;
  OverlayValueFn value;
  OverlayActionFn action;
  OverlayAdjustFn adjust;
} OverlayMenuItem;

static bool visible;
static bool toggleComboHeld;
static bool releaseCapture;
static bool textureDirty;
static uint32_t selectedItem;
static uint64_t lastPrepareTime;

static uint32_t streamWidth;
static uint32_t streamHeight;
static uint32_t streamFps;
static uint32_t streamBitrate;

static uint32_t decodedFrames;
static uint32_t displayedFrames;
static uint32_t skippedFrames;
static uint32_t droppedFrames;
static uint32_t decodedFps;
static uint32_t displayedFps;
static uint32_t skippedFps;
static uint32_t droppedFps;
static uint32_t queuedFrames;
static uint32_t frameNumber;
static uint32_t frameBytes;
static bool idrFrame;
static uint16_t hostProcessingLatency;
static uint32_t receiveLatencyMs;
static uint32_t decoderQueueMs;
static int connectionStatus = CONN_STATUS_OKAY;

#define OVERLAY_REFRESH_MS 1000

static void close_overlay(void) {
  visible = false;
  selectedItem = 0;
}

static void exit_app(void) {
  visible = false;
  wiiu_proc_request_exit();
  state = STATE_STOP_STREAM;
}

static void write_resume(char* buffer, size_t size) {
  snprintf(buffer, size, "Close overlay");
}

static void write_exit(char* buffer, size_t size) {
  snprintf(buffer, size, "Close app");
}

static void write_connection(char* buffer, size_t size) {
  const char* status = "Unknown";
  if (connectionStatus == CONN_STATUS_OKAY) {
    status = "Okay";
  }
  else if (connectionStatus == CONN_STATUS_POOR) {
    status = "Poor";
  }

  snprintf(buffer, size, "%s", status);
}

static void write_stream(char* buffer, size_t size) {
  if (streamFps && streamBitrate) {
    snprintf(buffer, size, "%ux%u @ %u fps, %u kbps", streamWidth, streamHeight, streamFps, streamBitrate);
  }
  else {
    snprintf(buffer, size, "%ux%u", streamWidth, streamHeight);
  }
}

static const char* filter_name(uint32_t mode) {
  switch (mode) {
    case WIIU_STREAM_FILTER_POINT:
      return "Point";
    case WIIU_STREAM_FILTER_LINEAR:
      return "Linear";
    case WIIU_STREAM_TV_OFF:
      return "Off";
  }

  return "Unknown";
}

static void write_tv_filter(char* buffer, size_t size) {
  snprintf(buffer, size, "%s", filter_name(wiiu_stream_get_tv_filter_mode()));
}

static void write_drc_filter(char* buffer, size_t size) {
  snprintf(buffer, size, "%s", filter_name(wiiu_stream_get_drc_filter_mode()));
}

static void write_frames(char* buffer, size_t size) {
  uint32_t skippedPercent = decodedFps ? (skippedFps * 100) / decodedFps : 0;
  uint32_t droppedPercent = decodedFps ? (droppedFps * 100) / decodedFps : 0;
  uint32_t lostPercent = decodedFps ? ((skippedFps + droppedFps) * 100) / decodedFps : 0;
  snprintf(buffer, size, "fps in %u, out %u, skip %u%%, drop %u%%, lost %u%%",
           decodedFps, displayedFps, skippedPercent, droppedPercent, lostPercent);
}

static void write_frame_totals(char* buffer, size_t size) {
  uint32_t skippedPercent = decodedFrames ? (skippedFrames * 100) / decodedFrames : 0;
  uint32_t droppedPercent = decodedFrames ? (droppedFrames * 100) / decodedFrames : 0;
  uint32_t lostPercent = decodedFrames ? ((skippedFrames + droppedFrames) * 100) / decodedFrames : 0;
  snprintf(buffer, size, "decoded %u, displayed %u, skipped %u%%, dropped %u%%, lost %u%%",
           decodedFrames, displayedFrames, skippedPercent, droppedPercent, lostPercent);
}

static void write_queue(char* buffer, size_t size) {
  int pendingVideo = LiGetPendingVideoFrames();
  int pendingAudioMs = LiGetPendingAudioDuration();
  snprintf(buffer, size, "display %u, video %d, audio %d ms", queuedFrames, pendingVideo, pendingAudioMs);
}

static void write_latency(char* buffer, size_t size) {
  uint32_t rtt;
  uint32_t rttVariance;

  if (LiGetEstimatedRttInfo(&rtt, &rttVariance)) {
    snprintf(buffer, size, "RTT %u ms (+/-%u), recv %u ms, decode queue %u ms",
             rtt, rttVariance, receiveLatencyMs, decoderQueueMs);
  }
  else {
    snprintf(buffer, size, "RTT n/a, recv %u ms, decode queue %u ms", receiveLatencyMs, decoderQueueMs);
  }
}

static void write_host(char* buffer, size_t size) {
  if (hostProcessingLatency == 0) {
    snprintf(buffer, size, "n/a");
  }
  else {
    snprintf(buffer, size, "%u.%u ms", hostProcessingLatency / 10, hostProcessingLatency % 10);
  }
}

static void write_frame(char* buffer, size_t size) {
  snprintf(buffer, size, "#%u, %u bytes, %s", frameNumber, frameBytes, idrFrame ? "IDR" : "P-frame");
}

static void write_input(char* buffer, size_t size) {
  snprintf(buffer, size, "LT + RT + PLUS toggles menu");
}

static uint32_t cycle_value(uint32_t value, uint32_t count, int delta) {
  if (delta > 0) {
    return (value + 1) % count;
  }

  return value == 0 ? count - 1 : value - 1;
}

static void adjust_tv_filter(int delta) {
  wiiu_stream_set_tv_filter_mode(cycle_value(wiiu_stream_get_tv_filter_mode(), WIIU_STREAM_TV_OFF + 1, delta));
}

static void adjust_drc_filter(int delta) {
  wiiu_stream_set_drc_filter_mode(cycle_value(wiiu_stream_get_drc_filter_mode(), WIIU_STREAM_FILTER_LINEAR + 1, delta));
}

static void next_tv_filter(void) {
  adjust_tv_filter(1);
}

static void next_drc_filter(void) {
  adjust_drc_filter(1);
}

static const OverlayMenuItem menuItems[] = {
  { "Resume",     write_resume,     close_overlay,    NULL },
  { "TV output",  write_tv_filter,  next_tv_filter,   adjust_tv_filter },
  { "DRC output", write_drc_filter, next_drc_filter,  adjust_drc_filter },
  { "Connection", write_connection, NULL,             NULL },
  { "Stream",     write_stream,     NULL,             NULL },
  { "Latency",    write_latency,    NULL,             NULL },
  { "Host frame", write_host,       NULL,             NULL },
  { "Last frame", write_frame,      NULL,             NULL },
  { "Frames",     write_frames,     NULL,             NULL },
  { "Totals",     write_frame_totals, NULL,           NULL },
  { "Queue",      write_queue,      NULL,             NULL },
  { "Input",      write_input,      NULL,             NULL },
  { "Exit",       write_exit,       exit_app,        NULL },
};

#define MENU_ITEM_COUNT (sizeof(menuItems) / sizeof(menuItems[0]))

void Overlay_Init(void) {
  visible = false;
  toggleComboHeld = false;
  releaseCapture = false;
  textureDirty = true;
  selectedItem = 0;
  lastPrepareTime = 0;
}

void Overlay_SetStreamConfig(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate) {
  streamWidth = width;
  streamHeight = height;
  streamFps = fps;
  streamBitrate = bitrate;
}

void Overlay_SetFrameStats(uint32_t decoded, uint32_t displayed, uint32_t skipped, uint32_t dropped,
                           uint32_t decodedRate, uint32_t displayedRate, uint32_t skippedRate, uint32_t droppedRate,
                           uint32_t queued) {
  decodedFrames = decoded;
  displayedFrames = displayed;
  skippedFrames = skipped;
  droppedFrames = dropped;
  decodedFps = decodedRate;
  displayedFps = displayedRate;
  skippedFps = skippedRate;
  droppedFps = droppedRate;
  queuedFrames = queued;
}

void Overlay_SetVideoStats(uint32_t frame, uint32_t bytes, bool isIdrFrame, uint16_t hostLatency,
                           uint32_t receiveLatency, uint32_t queueLatency) {
  frameNumber = frame;
  frameBytes = bytes;
  idrFrame = isIdrFrame;
  hostProcessingLatency = hostLatency;
  receiveLatencyMs = receiveLatency;
  decoderQueueMs = queueLatency;
}

void Overlay_SetConnectionStatus(int status) {
  connectionStatus = status;
}

bool Overlay_InputUpdate(uint32_t held, uint32_t triggered) {
  bool comboHeld = (held & (OVERLAY_INPUT_LT | OVERLAY_INPUT_RT | OVERLAY_INPUT_PLUS)) ==
                   (OVERLAY_INPUT_LT | OVERLAY_INPUT_RT | OVERLAY_INPUT_PLUS);
  bool toggled = comboHeld && !toggleComboHeld;
  toggleComboHeld = comboHeld;

  if (releaseCapture) {
    if (held == 0) {
      releaseCapture = false;
    }
    else {
      return true;
    }
  }

  if (toggled) {
    visible = !visible;
    selectedItem = 0;
    textureDirty = true;
    if (!visible) {
      releaseCapture = true;
    }
    return true;
  }

  if (!visible) {
    return false;
  }

  if (triggered & OVERLAY_INPUT_UP) {
    selectedItem = selectedItem == 0 ? MENU_ITEM_COUNT - 1 : selectedItem - 1;
    textureDirty = true;
  }
  else if (triggered & OVERLAY_INPUT_DOWN) {
    selectedItem = (selectedItem + 1) % MENU_ITEM_COUNT;
    textureDirty = true;
  }

  if ((triggered & OVERLAY_INPUT_A) && menuItems[selectedItem].action) {
    menuItems[selectedItem].action();
    textureDirty = true;
    if (!visible) {
      releaseCapture = true;
    }
  }
  else if ((triggered & OVERLAY_INPUT_LEFT) && menuItems[selectedItem].adjust) {
    menuItems[selectedItem].adjust(-1);
    textureDirty = true;
  }
  else if ((triggered & OVERLAY_INPUT_RIGHT) && menuItems[selectedItem].adjust) {
    menuItems[selectedItem].adjust(1);
    textureDirty = true;
  }
  else if (triggered & OVERLAY_INPUT_B) {
    close_overlay();
    releaseCapture = true;
    textureDirty = true;
  }

  return true;
}

void Overlay_Prepare(void) {
  if (!visible) {
    return;
  }

  uint64_t now = LiGetMillis();
  if (!textureDirty && now - lastPrepareTime < OVERLAY_REFRESH_MS) {
    return;
  }

  Font_SetVirtualSize(FONT_BUFFER_WIDTH, FONT_BUFFER_HEIGHT);
  Font_Clear();

  uint32_t panelHeight = 105 + MENU_ITEM_COUNT * 24;
  Font_FillRect(10, 12, 834, panelHeight, 0, 0, 0, 255);

  Font_SetSize(24);
  Font_SetColor(255, 255, 255, 255);
  Font_Print(22, 34, "Moonlight Wii U");

  Font_SetSize(15);
  Font_SetColor(205, 230, 255, 240);
  Font_Print(23, 58, "Overlay menu");

  Font_SetSize(15);
  uint32_t y = 86;
  for (uint32_t i = 0; i < MENU_ITEM_COUNT; i++) {
    char value[128];
    value[0] = '\0';
    if (menuItems[i].value) {
      menuItems[i].value(value, sizeof(value));
    }

    if (i == selectedItem) {
      Font_SetColor(125, 255, 190, 255);
      Font_Printf(23, y, "> %s", menuItems[i].label);
    }
    else {
      Font_SetColor(255, 255, 255, 240);
      Font_Printf(23, y, "  %s", menuItems[i].label);
    }

    Font_SetColor(235, 235, 235, 235);
    Font_Printf(190, y, "%s", value);
    y += 24;
  }

  Font_SetSize(13);
  Font_SetColor(215, 215, 215, 235);
  Font_Print(23, y + 18, "A selects/cycles, Left/Right adjusts, B closes. Host input is captured while visible.");

  textureDirty = false;
  lastPrepareTime = now;
  Font_SetVirtualSize(FONT_DEFAULT_VIRTUAL_WIDTH, FONT_DEFAULT_VIRTUAL_HEIGHT);
}

void Overlay_Draw(void) {
  if (visible) {
    Font_Draw();
  }
}
