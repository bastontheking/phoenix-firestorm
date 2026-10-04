/**
 * @file fsaislcontext.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsaislcontext.h"

#include "llagent.h"
#include "llavatarnamecache.h"
#include "llavatarpropertiesprocessor.h"
#include "llcallbacklist.h"
#include "llcallingcard.h"
#include "lleventtimer.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "rlvactions.h"

#include <boost/algorithm/string/join.hpp>
#include <set>

namespace
{
    constexpr F32    NEARBY_RADIUS = 256.f;  // meters
    constexpr size_t MAX_NEARBY_LISTED = 30;
    constexpr size_t MAX_FRIENDS_LISTED = 40;
    constexpr size_t MAX_PROFILES = 3;
    constexpr F32    PROFILE_TIMEOUT = 6.f;  // seconds

    struct Candidate
    {
        LLUUID      mID;
        std::string mDisplayName;
        std::string mUserName;
        F32         mDistance = -1.f; // < 0: not nearby
        bool        mFriend = false;
        bool        mOnline = false;
    };

    bool namesAllowed()
    {
        return RlvActions::canShowName(RlvActions::SNC_DEFAULT);
    }

    std::string describeName(const LLAvatarName& av_name)
    {
        const std::string display = av_name.getDisplayName(true);
        const std::string user = av_name.getUserName();
        if (display.empty() || LLStringUtil::compareInsensitive(display, user) == 0)
        {
            return user;
        }
        return display + " (" + user + ")";
    }

    // Avatars nearby and friends, with whatever names are already cached.
    std::vector<Candidate> collectCandidates()
    {
        std::map<LLUUID, Candidate> by_id;

        uuid_vec_t ids;
        std::vector<LLVector3d> positions;
        const LLVector3d my_pos = gAgent.getPositionGlobal();
        LLWorld::getInstance()->getAvatars(&ids, &positions, my_pos, NEARBY_RADIUS);
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (ids[i] == gAgentID)
            {
                continue;
            }
            Candidate& c = by_id[ids[i]];
            c.mID = ids[i];
            c.mDistance = (F32)dist_vec(positions[i], my_pos);
        }

        LLAvatarTracker::buddy_map_t buddies;
        LLAvatarTracker::instance().copyBuddyList(buddies);
        for (const auto& [id, relationship] : buddies)
        {
            Candidate& c = by_id[id];
            c.mID = id;
            c.mFriend = true;
            c.mOnline = LLAvatarTracker::instance().isBuddyOnline(id);
        }

        std::vector<Candidate> out;
        for (auto& [id, c] : by_id)
        {
            LLAvatarName av_name;
            if (LLAvatarNameCache::get(id, &av_name))
            {
                c.mDisplayName = av_name.getDisplayName(true);
                c.mUserName = av_name.getUserName();
                out.push_back(c);
            }
        }
        return out;
    }

    std::string lower(std::string s)
    {
        LLStringUtil::toLower(s);
        return s;
    }

    // Does the (lower-case) question mention this avatar?
    bool mentions(const std::string& question, const Candidate& c)
    {
        std::vector<std::string> keys;
        keys.push_back(lower(c.mDisplayName));
        keys.push_back(lower(c.mUserName));                       // "first last" or "first"
        std::string dotted = lower(c.mUserName);
        LLStringUtil::replaceString(dotted, " ", ".");
        keys.push_back(dotted);                                   // "first.last"
        // first words of display and user name, if distinctive enough
        for (const std::string& name : { lower(c.mDisplayName), lower(c.mUserName) })
        {
            const size_t space = name.find_first_of(" .");
            const std::string first = name.substr(0, space);
            if (first.size() >= 4)
            {
                keys.push_back(first);
            }
        }

        for (const std::string& key : keys)
        {
            if (key.size() < 3)
            {
                continue;
            }
            size_t pos = question.find(key);
            while (pos != std::string::npos)
            {
                // whole-word match
                const bool start_ok = pos == 0 || !isalnum((unsigned char)question[pos - 1]);
                const size_t end = pos + key.size();
                const bool end_ok = end >= question.size() || !isalnum((unsigned char)question[end]);
                if (start_ok && end_ok)
                {
                    return true;
                }
                pos = question.find(key, pos + 1);
            }
        }
        return false;
    }

    std::string nameOf(const LLUUID& id)
    {
        LLAvatarName av_name;
        return LLAvatarNameCache::get(id, &av_name) ? describeName(av_name) : std::string("(unknown)");
    }

    std::string summarizeProfile(const Candidate& c, const LLAvatarData* data)
    {
        std::string out = "Profile of " + (c.mDisplayName.empty() || LLStringUtil::compareInsensitive(c.mDisplayName, c.mUserName) == 0
                                             ? c.mUserName : c.mDisplayName + " (" + c.mUserName + ")");
        std::vector<std::string> where;
        if (c.mDistance >= 0.f)
        {
            where.push_back(llformat("nearby, %.0f m away", c.mDistance));
        }
        if (c.mFriend)
        {
            where.push_back(c.mOnline ? "your friend, online" : "your friend, offline");
        }
        if (!where.empty())
        {
            out += " [" + boost::algorithm::join(where, "; ") + "]";
        }
        out += ":\n";

        if (!data)
        {
            return out + "- (profile could not be loaded)\n";
        }
        if (!data->hide_age && data->born_on.secondsSinceEpoch() > 0)
        {
            const std::string born = data->born_on.asString().substr(0, 10);
            const F64 years = (LLDate::now().secondsSinceEpoch() - data->born_on.secondsSinceEpoch()) / (365.25 * 24 * 3600);
            out += llformat("- In Second Life since %s (about %.1f years)\n", born.c_str(), years);
        }
        out += "- Partner: " + (data->partner_id.notNull() ? nameOf(data->partner_id) : std::string("none")) + "\n";
        if (!data->group_list.empty())
        {
            std::vector<std::string> groups;
            for (const auto& group : data->group_list)
            {
                if (groups.size() >= 12)
                {
                    break;
                }
                groups.push_back(group.group_name);
            }
            out += llformat("- Groups (%d): ", (S32)data->group_list.size()) + boost::algorithm::join(groups, ", ") + "\n";
        }
        if (!data->picks_list.empty())
        {
            std::vector<std::string> picks;
            for (const auto& pick : data->picks_list)
            {
                if (picks.size() >= 6)
                {
                    break;
                }
                picks.push_back(pick.second);
            }
            out += "- Picks: " + boost::algorithm::join(picks, ", ") + "\n";
        }
        std::string about = data->about_text;
        LLStringUtil::trim(about);
        if (!about.empty())
        {
            if (about.size() > 900)
            {
                about = about.substr(0, 900) + "...";
            }
            out += "- About (their own words): " + about + "\n";
        }
        if (!data->profile_url.empty())
        {
            out += "- Web profile: " + data->profile_url + "\n";
        }
        return out;
    }

    // One batch of profile requests; owns itself until finished.
    class ProfileFetch : public LLAvatarPropertiesObserver, public std::enable_shared_from_this<ProfileFetch>
    {
    public:
        ProfileFetch(std::vector<Candidate> targets, std::function<void(const std::string&)> callback)
        :   mTargets(std::move(targets)), mCallback(std::move(callback))
        {
        }

        void start()
        {
            sActive.insert(shared_from_this());
            for (const Candidate& c : mTargets)
            {
                LLAvatarPropertiesProcessor::getInstance()->addObserver(c.mID, this);
                LLAvatarPropertiesProcessor::getInstance()->sendAvatarPropertiesRequest(c.mID);
            }
            std::weak_ptr<ProfileFetch> weak = shared_from_this();
            LLEventTimer::run_after(PROFILE_TIMEOUT, [weak]()
            {
                if (auto self = weak.lock())
                {
                    self->finish();
                }
            });
        }

        void processProperties(void* data, EAvatarProcessorType type) override
        {
            if (mFinished || (type != APT_PROPERTIES && type != APT_PROPERTIES_LEGACY) || !data)
            {
                return;
            }
            const LLAvatarData* avatar_data = static_cast<const LLAvatarData*>(data);
            for (const Candidate& c : mTargets)
            {
                if (c.mID == avatar_data->avatar_id && !mResults.count(c.mID))
                {
                    mResults[c.mID] = summarizeProfile(c, avatar_data);
                }
            }
            if (mResults.size() >= mTargets.size())
            {
                // finish outside of the processor's notification loop
                std::weak_ptr<ProfileFetch> weak = shared_from_this();
                doOnIdleOneTime([weak]()
                {
                    if (auto self = weak.lock())
                    {
                        self->finish();
                    }
                });
            }
        }

    private:
        void finish()
        {
            if (mFinished)
            {
                return;
            }
            mFinished = true;
            std::string text;
            for (const Candidate& c : mTargets)
            {
                LLAvatarPropertiesProcessor::getInstance()->removeObserver(c.mID, this);
                auto it = mResults.find(c.mID);
                text += (it != mResults.end()) ? it->second : summarizeProfile(c, nullptr);
                text += "\n";
            }
            mCallback(text);
            sActive.erase(shared_from_this()); // may delete this
        }

        std::vector<Candidate> mTargets;
        std::map<LLUUID, std::string> mResults;
        std::function<void(const std::string&)> mCallback;
        bool mFinished = false;

        static std::set<std::shared_ptr<ProfileFetch>> sActive;
    };

    std::set<std::shared_ptr<ProfileFetch>> ProfileFetch::sActive;
}

// static
std::string FSAISLContext::buildSceneContext()
{
    if (gAgentID.isNull())
    {
        return std::string();
    }

    std::string out = "Live Second Life context from the user's viewer (use it when relevant, do not recite it unprompted):\n";
    const bool names = namesAllowed();

    LLAvatarName me;
    if (names && LLAvatarNameCache::get(gAgentID, &me))
    {
        out += "- The user's avatar: " + describeName(me) + "\n";
    }
    if (RlvActions::canShowLocation() && gAgent.getRegion())
    {
        const LLVector3 pos = gAgent.getPositionAgent();
        out += llformat("- Region: %s (position %.0f, %.0f, %.0f)\n", gAgent.getRegion()->getName().c_str(), pos.mV[VX], pos.mV[VY], pos.mV[VZ]);
    }
    if (!names)
    {
        return out + "- Avatar names are currently restricted (RLV), so no names are available.\n";
    }

    std::vector<Candidate> candidates = collectCandidates();

    std::vector<Candidate> nearby;
    std::vector<Candidate> friends_online;
    for (const Candidate& c : candidates)
    {
        if (c.mDistance >= 0.f)
        {
            nearby.push_back(c);
        }
        if (c.mFriend && c.mOnline)
        {
            friends_online.push_back(c);
        }
    }
    std::sort(nearby.begin(), nearby.end(), [](const Candidate& a, const Candidate& b) { return a.mDistance < b.mDistance; });

    out += llformat("- Avatars within %.0f m (%d):", NEARBY_RADIUS, (S32)nearby.size());
    for (size_t i = 0; i < nearby.size() && i < MAX_NEARBY_LISTED; ++i)
    {
        const Candidate& c = nearby[i];
        out += (i ? "; " : " ") + (LLStringUtil::compareInsensitive(c.mDisplayName, c.mUserName) == 0 ? c.mUserName : c.mDisplayName + " (" + c.mUserName + ")")
             + llformat(" %.0f m", c.mDistance) + (c.mFriend ? " [friend]" : "");
    }
    out += "\n";

    if (!friends_online.empty())
    {
        out += llformat("- Friends online (%d):", (S32)friends_online.size());
        for (size_t i = 0; i < friends_online.size() && i < MAX_FRIENDS_LISTED; ++i)
        {
            out += (i ? ", " : " ") + friends_online[i].mDisplayName;
        }
        out += "\n";
    }
    return out;
}

// static
void FSAISLContext::fetchProfiles(const std::string& question, std::function<void(const std::string&)> callback)
{
    if (!namesAllowed())
    {
        callback(std::string());
        return;
    }

    const std::string q = lower(question);
    std::vector<Candidate> mentioned;
    std::vector<Candidate> candidates = collectCandidates();
    // closest first, so ambiguous first names prefer people around the user
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
    {
        const F32 da = a.mDistance < 0.f ? FLT_MAX : a.mDistance;
        const F32 db = b.mDistance < 0.f ? FLT_MAX : b.mDistance;
        return da < db;
    });
    for (const Candidate& c : candidates)
    {
        if (mentioned.size() >= MAX_PROFILES)
        {
            break;
        }
        if (mentions(q, c))
        {
            mentioned.push_back(c);
        }
    }

    if (mentioned.empty())
    {
        callback(std::string());
        return;
    }
    auto fetch = std::make_shared<ProfileFetch>(std::move(mentioned), std::move(callback));
    fetch->start();
}
