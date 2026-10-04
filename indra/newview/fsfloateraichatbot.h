/**
 * @file fsfloateraichatbot.h
 * @brief Chat bot tabs in the conversations window, backed by the user's own LLM
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
class LLLineEditor;
class LLTextBox;
class LLTextEditor;

/**
 * A private conversation with the user's LLM, shown as tabs right after
 * Nearby Chat. Nothing typed here goes to Second Life.
 *
 * Two instances share this class:
 *  - "fs_ai_chatbot": every message is answered on its own, nothing is
 *    remembered;
 *  - "fs_ai_chatbot_history": the conversation is kept per account in
 *    ai_chatbot_history.xml and the last FSAIChatbotHistoryLength entries
 *    are sent as context.
 */
class FSFloaterAIChatbot : public LLFloater
{
public:
    static constexpr const char* SINGLE_NAME = "fs_ai_chatbot";
    static constexpr const char* HISTORY_NAME = "fs_ai_chatbot_history";

    FSFloaterAIChatbot(const LLSD& key, bool use_history = false);
    ~FSFloaterAIChatbot() override = default;

    bool postBuild() override;

    // Erase the remembered conversation (settings floater button).
    static void clearSavedHistory();

private:
    struct Entry
    {
        std::string mRole;     // "user" or "assistant"
        std::string mContent;
    };

    bool usesHistory() const { return mUseHistory; }
    void onSend();
    void onClear();
    void askModel(const std::string& question, const std::string& web_context, const std::string& sources);
    void appendEntry(const Entry& entry);
    void appendLine(const std::string& text, const LLColor4& color, bool italic = false);
    void showWelcome();
    void setBusy(bool busy, const std::string& status = LLStringUtil::null);
    void loadHistory();
    void saveHistory() const;
    static std::string historyFile();

    std::vector<Entry> mHistory;
    bool               mBusy = false;
    const bool         mUseHistory;

    LLTextEditor*   mChatHistory = nullptr;
    LLLineEditor*   mInput = nullptr;
    LLButton*       mSendBtn = nullptr;
    LLTextBox*      mStatusText = nullptr;
};

// The remembered conversation ("fs_ai_chatbot_history").
class FSFloaterAIChatbotHistory : public FSFloaterAIChatbot
{
public:
    FSFloaterAIChatbotHistory(const LLSD& key) : FSFloaterAIChatbot(key, true) {}
};

#endif // FS_FLOATERAICHATBOT_H
