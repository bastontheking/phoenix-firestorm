/**
 * @file llframebudget.cpp
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

#include "llviewerprecompiledheaders.h"

#include "llframebudget.h"

#include "llviewercontrol.h"

F32 LLFrameBudget::sTargetFrameSeconds = 1.f / 60.f;
F32 LLFrameBudget::sSmoothedFrameSeconds = 1.f / 60.f;
F32 LLFrameBudget::sPressure = 0.f;

namespace
{
    constexpr F32 SMOOTHING = 0.1f;          // EMA weight of the newest frame
    constexpr F32 OVER_BUDGET = 1.05f;       // start degrading above 105% of budget
    constexpr F32 UNDER_BUDGET = 0.85f;      // start restoring below 85% of budget
    constexpr F32 RECOVERY_PER_SECOND = 0.1f; // full recovery takes ~10 s of headroom
}

// static
void LLFrameBudget::update(F32 frame_seconds)
{
    static LLCachedControl<F32> target_fps(gSavedSettings, "FSFrameBudgetTargetFPS", 60.f);
    static LLCachedControl<bool> adaptive(gSavedSettings, "FSAdaptiveQuality", true);

    sTargetFrameSeconds = 1.f / llclamp((F32)target_fps, 15.f, 500.f);

    if (frame_seconds <= 0.f)
    {
        return;
    }

    // A single huge hitch (teleport, window drag, loading) must not throw
    // the controller to maximum pressure: clamp what one frame contributes.
    const F32 sample = llmin(frame_seconds, sTargetFrameSeconds * 4.f);
    sSmoothedFrameSeconds = llmax(sSmoothedFrameSeconds + (sample - sSmoothedFrameSeconds) * SMOOTHING, 0.0001f);

    if (!adaptive)
    {
        sPressure = 0.f;
        return;
    }

    const F32 ratio = sSmoothedFrameSeconds / sTargetFrameSeconds;
    const F32 dt = llmin(frame_seconds, 0.1f);
    if (ratio > OVER_BUDGET)
    {
        // React faster the further we are over budget.
        sPressure += dt * (0.25f + llmin(ratio - 1.f, 2.f));
    }
    else if (ratio < UNDER_BUDGET)
    {
        sPressure -= dt * RECOVERY_PER_SECOND;
    }
    sPressure = llclamp(sPressure, 0.f, 1.f);
}

// static
F32 LLFrameBudget::getGeomUpdateBudget(F32 requested_seconds)
{
    static LLCachedControl<F32> min_budget_ms(gSavedSettings, "FSGeomUpdateMinBudgetMs", 2.f);
    const F32 floor_seconds = llmax((F32)min_budget_ms, 0.25f) * 0.001f;
    // Under pressure, spend less of the frame on catching up rebuilds, but
    // never less than half of the floor so the queue keeps draining.
    const F32 budget = llmax(requested_seconds, floor_seconds) * (1.f - 0.5f * sPressure);
    return llmax(budget, floor_seconds * 0.5f);
}

// static
S32 LLFrameBudget::getMaxImpostorUpdates()
{
    static LLCachedControl<U32> max_updates(gSavedSettings, "FSMaxImpostorUpdatesPerFrame", 8);
    const S32 base = llmax((S32)(U32)max_updates, 1);
    // 8 -> 2 at full pressure
    return llmax(1, (S32)ll_round(base * (1.f - 0.75f * sPressure)));
}

// static
U32 LLFrameBudget::getAvatarAnimPeriod(F32 screen_fraction)
{
    static LLCachedControl<bool> anim_lod(gSavedSettings, "FSAvatarAnimLOD", true);
    if (!anim_lod)
    {
        return 1;
    }

    // At zero pressure only avatars that are genuinely small on screen
    // animate at a reduced rate (30 Hz / 20 Hz at 60 FPS), which is not
    // perceptible at that size. Under pressure the tiers widen.
    const F32 scale = 1.f + 2.f * sPressure;
    if (screen_fraction >= 0.10f * scale)
    {
        return 1;
    }
    if (screen_fraction >= 0.04f * scale)
    {
        return 2;
    }
    if (screen_fraction >= 0.015f * scale)
    {
        return 3;
    }
    return 4 + (U32)ll_round(2.f * sPressure);
}

// static
F32 LLFrameBudget::getSmallObjectLODScale(F32 projected_tan)
{
    if (sPressure <= 0.f)
    {
        return 1.f;
    }
    // LLVolumeLODGroup thresholds are 0.03 / 0.06 / 0.24. Objects at or
    // above the high LOD threshold are never touched; objects at medium/low
    // size lose up to 35% of their LOD factor under full pressure.
    constexpr F32 SMALL = 0.06f;
    constexpr F32 LARGE = 0.24f;
    if (projected_tan >= LARGE)
    {
        return 1.f;
    }
    const F32 t = llclamp((projected_tan - SMALL) / (LARGE - SMALL), 0.f, 1.f);
    const F32 max_reduction = 0.35f * sPressure;
    return 1.f - max_reduction * (1.f - t);
}
