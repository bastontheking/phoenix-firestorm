/**
 * @file fsfloateraichatbot.h
 * @brief Chat bot tab in the conversations window, backed by the user's own LLM
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

#ifndef FS_FLOATERAICHATBOT_H
#define FS_FLOATERAICHATBOT_H

#include "llfloater.h"

class LLButton;
class LLCheckBoxCtrl;
class LLLineEditor;
class LLTextBox;
class LLTextEditor;

/**
 * A private conversation with the user's LLM, shown as a tab right after
 * Nearby Chat. Nothing typed here goes to Second Life unless the user
 * presses "Send to chat".
 *
 * History is kept per account in ai_chatbot_history.json; the last
 * FSAIChatbotHistoryLength entries are sent as context.
 */
class FSFloaterAIChatbot : public LLFloater
{
public:
    FSFloaterAIChatbot(const LLSD& key);
    ~FSFloaterAIChatbot() override = default;

    bool postBuild() override;

private:
    struct Entry
    {
        std::string mRole;     // "user" or "assistant"
        std::string mContent;
        bool        mOneOff = false; // never used as context
    };

    void onSend();
    void onSendToChat();
    void onClear();
    void askModel(const std::string& question, const std::string& web_context, bool one_off, const std::string& sources);
    void appendEntry(const Entry& entry);
    void appendLine(const std::string& text, const LLColor4& color, bool italic = false);
    void setBusy(bool busy, const std::string& status = LLStringUtil::null);
    void loadHistory();
    void saveHistory() const;
    std::string historyFile() const;

    std::vector<Entry> mHistory;
    bool               mBusy = false;

    LLTextEditor*   mChatHistory = nullptr;
    LLLineEditor*   mInput = nullptr;
    LLButton*       mSendBtn = nullptr;
    LLCheckBoxCtrl* mOneOffCheck = nullptr;
    LLCheckBoxCtrl* mWebSearchCheck = nullptr;
    LLTextBox*      mStatusText = nullptr;
};

#endif // FS_FLOATERAICHATBOT_H
