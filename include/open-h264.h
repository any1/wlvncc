#pragma once

#define OPEN_H264_MAX_CONTEXTS 64

#include <stdbool.h>
#include <rfb/rfbclient.h>

struct AVFrame;
struct open_h264;
struct open_h264_context;

struct open_h264* open_h264_create(rfbClient* client);
void open_h264_destroy(struct open_h264*);

struct AVFrame* open_h264_decode_rect(struct open_h264* self,
		rfbFramebufferUpdateRectHeader* message);
