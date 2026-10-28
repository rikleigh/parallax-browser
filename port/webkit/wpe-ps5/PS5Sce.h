/*
 * Copyright (C) 2026 ps5webkit contributors
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.
 */

// Sony system library entry points used by the PS5 WPE platform (title side).
// ps5-payload-sdk ships link stubs (libScePad.so, libSceImeDialog.so,
// libSceUserService.so) but no headers, so the declarations are here. They
// are weak: the same WebKit objects are linked into the payload child
// (WebProcess/NetworkProcess), which never calls them and does not link the
// stubs, and the title checks for NULL before use.

#pragma once

#include <stdint.h>

extern "C" {

// libSceUserService
int sceUserServiceInitialize(const void* params) __attribute__((weak));
int sceUserServiceGetInitialUser(int32_t* userID) __attribute__((weak));
int sceUserServiceGetForegroundUser(int32_t* userID) __attribute__((weak));

// libScePad. ScePadData as PS5VR reads it (app/src/app_input.c); the buffer
// handed to scePadReadState is larger than the real struct on purpose.
int scePadInit(void) __attribute__((weak));
int scePadOpen(int32_t userID, int32_t type, int32_t index, const void* param) __attribute__((weak));
int scePadClose(int32_t handle) __attribute__((weak));
int scePadReadState(int32_t handle, void* data) __attribute__((weak));

struct PS5PadData {
    uint32_t buttons;
    uint8_t sticks[4]; // left x, left y, right x, right y; 128 = centre
    uint8_t analog[4]; // L2, R2
    float orientation[4];
    float acceleration[3];
    float angularVelocity[3];
    uint8_t touch[24];
    uint8_t connected;
    uint8_t rest[191];
};

enum : uint32_t {
    PS5_PAD_L3 = 0x00000002,
    PS5_PAD_R3 = 0x00000004,
    PS5_PAD_OPTIONS = 0x00000008,
    PS5_PAD_UP = 0x00000010,
    PS5_PAD_RIGHT = 0x00000020,
    PS5_PAD_DOWN = 0x00000040,
    PS5_PAD_LEFT = 0x00000080,
    PS5_PAD_L2 = 0x00000100,
    PS5_PAD_R2 = 0x00000200,
    PS5_PAD_L1 = 0x00000400,
    PS5_PAD_R1 = 0x00000800,
    PS5_PAD_TRIANGLE = 0x00001000,
    PS5_PAD_CIRCLE = 0x00002000,
    PS5_PAD_CROSS = 0x00004000,
    PS5_PAD_SQUARE = 0x00008000,
    PS5_PAD_TOUCHPAD = 0x00100000,
};

// libSceImeDialog (PS4-compatible layout, as OpenOrbis documents it; text is
// UTF-16 because wchar_t is 16-bit on this target). The system does not load
// libSceImeDialog.sprx for the title, so its imports would stay NULL: the
// title loads it at run time and binds these (apps/title/titleime.c, title
// only; the weak reference is NULL in the payloads).
struct PS5ImeFunctions {
    int (*init)(const void* param, const void* extended);
    int (*getStatus)(void);
    int (*getResult)(void* result);
    int (*term)(void);
    int (*abort)(void);
    int (*commonDialogInitialize)(void);
};
int ps5wk_title_load_ime(PS5ImeFunctions*) __attribute__((weak));

enum PS5ImeType : int32_t {
    PS5_IME_TYPE_DEFAULT = 0,
    PS5_IME_TYPE_BASIC_LATIN = 1,
    PS5_IME_TYPE_URL = 2,
    PS5_IME_TYPE_MAIL = 3,
    PS5_IME_TYPE_NUMBER = 4,
};

enum PS5ImeEnterLabel : int32_t {
    PS5_IME_ENTER_LABEL_DEFAULT = 0,
    PS5_IME_ENTER_LABEL_SEND = 1,
    PS5_IME_ENTER_LABEL_SEARCH = 2,
    PS5_IME_ENTER_LABEL_GO = 3,
};

enum : uint32_t {
    PS5_IME_OPTION_MULTILINE = 0x01,
    PS5_IME_OPTION_NO_AUTO_CAPITALIZATION = 0x02,
    PS5_IME_OPTION_PASSWORD = 0x04,
};

struct PS5ImeDialogParam {
    int32_t userID;
    int32_t type;
    uint64_t supportedLanguages;
    int32_t enterLabel;
    int32_t inputMethod;
    void* filter;
    uint32_t option;
    uint32_t maxTextLength;
    char16_t* inputTextBuffer;
    float posX;
    float posY;
    int32_t horizontalAlignment;
    int32_t verticalAlignment;
    const char16_t* placeholder;
    const char16_t* title;
    int8_t reserved[16];
};
static_assert(sizeof(PS5ImeDialogParam) == 96, "SceImeDialogParam is 96 bytes");

enum PS5ImeDialogStatus : int32_t {
    PS5_IME_DIALOG_STATUS_NONE = 0,
    PS5_IME_DIALOG_STATUS_RUNNING = 1,
    PS5_IME_DIALOG_STATUS_FINISHED = 2,
};

struct PS5ImeDialogResult {
    int32_t endStatus; // 0 OK, 1 canceled, 2 aborted
    int8_t reserved[12];
};

// libkernel: TV popups, useful to see that the platform started.
struct PS5NotificationRequest {
    char unused[45];
    char message[3075];
};
int sceKernelSendNotificationRequest(int, PS5NotificationRequest*, size_t, int) __attribute__((weak));

} // extern "C"
