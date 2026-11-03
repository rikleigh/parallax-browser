/*
 * A small QR code encoder (ISO/IEC 18004): byte mode, error correction
 * level M, versions 1-10 (up to 213 bytes), the mask with the lowest
 * penalty. For the link server's address on the TV (ps5browser.c).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define QR_MAX_SIZE 57 /* version 10: 17 + 4 * 10 */

/* Encodes text (length bytes). modules: QR_MAX_SIZE * QR_MAX_SIZE bytes,
 * row-major with a stride of *size, 1 = dark. Returns the size (21-57),
 * 0 if the text is too long. No quiet zone (draw 4 modules around it). */
int qr_encode(const char* text, size_t length, uint8_t* modules);
