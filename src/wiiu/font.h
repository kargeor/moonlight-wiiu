#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>

#define GX2_CLEAR_COLOR 0.2f, 0.13f, 0.38f, 1.0f

#define FONT_DEFAULT_VIRTUAL_WIDTH 1920
#define FONT_DEFAULT_VIRTUAL_HEIGHT 1080
#define FONT_BUFFER_WIDTH 854
#define FONT_BUFFER_HEIGHT 480

void Font_Init(void);

void Font_Deinit(void);

void Font_Draw(void);

void Font_Draw_TVDRC(void);

void Font_Clear(void);

void Font_FillRect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

void Font_SetVirtualSize(uint32_t width, uint32_t height);

void Font_Printw(uint32_t x, uint32_t y, const wchar_t* string);

void Font_Print(uint32_t x, uint32_t y, const char* string);

void Font_Printf(uint32_t x, uint32_t y, const char* msg, ...);

void Font_SetColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

void Font_SetSize(uint32_t size);
