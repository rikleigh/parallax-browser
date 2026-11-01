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

#pragma once

// Spike S1a (port/webkit/COMPOSITING-PLAN.md): 512x512 BGRA tile upload
// microbenchmark. Runs once on the render thread, in PS5Renderer's GL
// context, when /data/ps5webkit/glbench exists; logs "[glbench]" lines.
// The file may list method names (one per line or space separated) to run
// only those; empty means all.
void ps5GLBenchMaybeRun(int screenWidth, int screenHeight);
