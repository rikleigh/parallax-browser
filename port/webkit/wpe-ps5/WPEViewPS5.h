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

#ifndef WPEViewPS5_h
#define WPEViewPS5_h

#if !defined(__WPE_PS5_H_INSIDE__) && !defined(BUILDING_WEBKIT)
#error "Only <wpe/ps5/wpe-ps5.h> can be included directly."
#endif

#include <glib-object.h>
#include <wpe/wpe-platform.h>

G_BEGIN_DECLS

#define WPE_TYPE_VIEW_PS5 (wpe_view_ps5_get_type())
WPE_API G_DECLARE_FINAL_TYPE (WPEViewPS5, wpe_view_ps5, WPE, VIEW_PS5, WPEView)

G_END_DECLS

#endif /* WPEViewPS5_h */
