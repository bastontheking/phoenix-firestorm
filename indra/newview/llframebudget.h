/**
 * @file llframebudget.h
 * @brief Frame-time budget controller driving perceptually cheap quality knobs.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLFRAMEBUDGET_H
#define LL_LLFRAMEBUDGET_H

#include "stdtypes.h"

/**
 * LLFrameBudget is the single place that decides how much per-frame work the
 * viewer may spend on deferrable or perceptually minor tasks.
 *
 * Every frame it is fed the measured frame time. It keeps a smoothed frame
 * time and a "pressure" value in [0, 1]:
 *   - pressure rises quickly while frames exceed the budget,
 *   - and falls slowly (hysteresis) once there is headroom, so quality is
 *     restored gradually and does not oscillate.
 *
 * Consumers query derived knobs instead of reading settings directly. The
 * knobs are ordered so that the least visible things degrade first, and none
 * of them touches objects that are large on screen:
 *   1. animation update rate of avatars that are small on screen,
 *   2. LOD of objects that are small on screen,
 *   3. impostor refreshes per frame,
 *   4. time spent per frame on deferrable geometry rebuilds.
 *
 * When FSAdaptiveQuality is off, pressure stays 0 and every knob returns its
 * full-quality value; the frame-time measurements are still collected.
 */
class LLFrameBudget
{
public:
    // Called once per frame from the main loop with the last frame's duration.
    static void update(F32 frame_seconds);

    static F32 getTargetFrameSeconds() { return sTargetFrameSeconds; }
    static F32 getSmoothedFrameSeconds() { return sSmoothedFrameSeconds; }
    static F32 getPressure() { return sPressure; }

    // Seconds the main thread may spend on deferrable geometry rebuilds
    // this frame (LLPipeline::updateGeom). Never less than a fixed floor so
    // the queue always makes progress.
    static F32 getGeomUpdateBudget(F32 requested_seconds);

    // How many avatar impostors may be regenerated this frame.
    static S32 getMaxImpostorUpdates();

    // Animation update period (in frames) for an avatar whose height
    // covers screen_fraction of the viewport height. 1 == every frame.
    static U32 getAvatarAnimPeriod(F32 screen_fraction);

    // Multiplier (<= 1) applied to the LOD factor of an object with the
    // given projected size (tan of the half angle it subtends). Large objects
    // always get 1.
    static F32 getSmallObjectLODScale(F32 projected_tan);

private:
    static F32 sTargetFrameSeconds;
    static F32 sSmoothedFrameSeconds;
    static F32 sPressure;
};

#endif // LL_LLFRAMEBUDGET_H
