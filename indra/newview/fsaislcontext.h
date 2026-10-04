/**
 * @file fsaislcontext.h
 * @brief Second Life context (scene, nearby avatars, profiles) for the AI ChatBot
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
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

#ifndef FS_AISLCONTEXT_H
#define FS_AISLCONTEXT_H

#include <functional>
#include <string>

/**
 * Gives the ChatBot knowledge of the user's Second Life surroundings:
 *  - buildSceneContext(): region, the user's avatar, nearby avatars with
 *    distances and online friends (data the viewer already has);
 *  - fetchProfiles(): when a question mentions a nearby avatar or a friend
 *    by display name, username or first name, their profile (bio, account
 *    date, partner, groups, picks) is requested and summarized.
 *
 * Respects RLVa name restrictions: when names may not be shown, no names or
 * profiles are given to the AI.
 */
class FSAISLContext
{
public:
    static std::string buildSceneContext();

    // Calls back (on the main thread) with a text block describing the
    // profiles of the avatars mentioned in question, or an empty string.
    static void fetchProfiles(const std::string& question, std::function<void(const std::string&)> callback);
};

#endif // FS_AISLCONTEXT_H
