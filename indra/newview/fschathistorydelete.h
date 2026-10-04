/**
 * @file fschathistorydelete.h
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

#ifndef FS_CHATHISTORYDELETE_H
#define FS_CHATHISTORYDELETE_H

#include "lluuid.h"

#include <string>

/**
 * "Delete chat history" for a single person or group: asks for confirmation,
 * then removes that conversation's transcript file and its backups from disk
 * and clears the conversation window if it is open. Other conversations are
 * not touched (Preferences > Privacy > Delete transcripts deletes all).
 */
namespace FSChatHistoryDelete
{
    // For a person (contacts, radar, name lists...).
    void deleteForAvatar(const LLUUID& avatar_id);

    // For a known conversation: history_file is the transcript base name
    // as used by LLLogChat (LLConversation::getHistoryFileName(),
    // LLIMModel::getHistoryFileName()), session_id the IM session (may be null).
    void deleteForConversation(const std::string& history_file, const std::string& display_name, const LLUUID& session_id);
}

#endif // FS_CHATHISTORYDELETE_H
