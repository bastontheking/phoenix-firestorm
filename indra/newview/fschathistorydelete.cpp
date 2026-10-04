/**
 * @file fschathistorydelete.cpp
 * @brief Delete the saved chat history (transcript) of one conversation
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Baston (baston.dev)
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

#include "fschathistorydelete.h"

#include "fsfloaterim.h"
#include "llavatarnamecache.h"
#include "llcachename.h"
#include "llconversationlog.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "llimview.h"
#include "lllogchat.h"
#include "llnotificationsutil.h"
#include "llviewercontrol.h"

namespace
{
    // Transcript base name for a person, same rules as the rest of the
    // viewer (see LLAvatarActions::viewChatHistoryExternally).
    std::string historyFileForAvatar(const LLUUID& avatar_id, const LLAvatarName& av_name)
    {
        // Prefer the name recorded in the conversation log: it is exactly
        // the file the conversation was written to.
        for (const LLConversation& conversation : LLConversationLog::instance().getConversations())
        {
            if (conversation.getParticipantID() == avatar_id && !conversation.getHistoryFileName().empty())
            {
                return conversation.getHistoryFileName();
            }
        }
        if (gSavedSettings.getBOOL("UseLegacyIMLogNames"))
        {
            const std::string user_name = av_name.getUserName();
            return user_name.substr(0, user_name.find(" Resident"));
        }
        return LLCacheName::buildUsername(av_name.getUserName());
    }

    // The transcript and its rotated backups ("<name>.txt.backup...").
    std::vector<std::string> transcriptFiles(const std::string& history_file)
    {
        std::vector<std::string> files;
        const std::string main_file = LLLogChat::makeLogFileName(history_file);
        const std::string dir = gDirUtilp->getDirName(main_file);
        const std::string base = gDirUtilp->getBaseFileName(main_file);
        if (dir.empty() || base.empty())
        {
            return files;
        }
        // Compare names ourselves: chat names may contain glob characters.
        LLDirIterator iter(dir, "*");
        std::string name;
        while (iter.next(name))
        {
            if (LLStringUtil::compareInsensitive(name, base) == 0
                || LLStringUtil::startsWith(name, base + ".backup"))
            {
                files.push_back(gDirUtilp->add(dir, name));
            }
        }
        return files;
    }

    void doDelete(const std::string& history_file, const std::string& display_name, const LLUUID& session_id)
    {
        S32 deleted = 0;
        for (const std::string& file : transcriptFiles(history_file))
        {
            if (LLFile::remove(file) == 0)
            {
                ++deleted;
            }
            else
            {
                LL_WARNS("ChatHistory") << "Could not delete " << file << LL_ENDL;
            }
        }
        LL_INFOS("ChatHistory") << "Deleted " << deleted << " transcript file(s) for one conversation" << LL_ENDL;

        // Clear the open conversation window: reloading the session from
        // the (now missing) transcript leaves only this session's messages
        // out of the picture as well.
        if (session_id.notNull())
        {
            if (FSFloaterIM* floater = FSFloaterIM::findInstance(session_id))
            {
                floater->reloadMessages(true);
            }
            else if (LLIMModel::LLIMSession* session = LLIMModel::instance().findIMSession(session_id))
            {
                session->loadHistory();
            }
        }

        LLSD args;
        args["NAME"] = display_name;
        LLNotificationsUtil::add("FSDeleteChatHistoryDone", args);
    }
}

namespace FSChatHistoryDelete
{
    void deleteForConversation(const std::string& history_file, const std::string& display_name, const LLUUID& session_id)
    {
        LLSD args;
        args["NAME"] = display_name;
        if (history_file.empty() || transcriptFiles(history_file).empty())
        {
            LLNotificationsUtil::add("FSDeleteChatHistoryNone", args);
            return;
        }
        LLNotificationsUtil::add("FSDeleteChatHistoryConfirm", args, LLSD(),
            [history_file, display_name, session_id](const LLSD& notification, const LLSD& response)
            {
                if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
                {
                    doDelete(history_file, display_name, session_id);
                }
            });
    }

    void deleteForAvatar(const LLUUID& avatar_id)
    {
        if (avatar_id.isNull())
        {
            return;
        }
        LLAvatarName av_name;
        if (!LLAvatarNameCache::get(avatar_id, &av_name))
        {
            // Name not resolved yet: retry once it is.
            LLAvatarNameCache::get(avatar_id, [](const LLUUID& id, const LLAvatarName&) { deleteForAvatar(id); });
            return;
        }
        const LLUUID session_id = LLIMMgr::computeSessionID(IM_NOTHING_SPECIAL, avatar_id);
        deleteForConversation(historyFileForAvatar(avatar_id, av_name), av_name.getCompleteName(), session_id);
    }
}
