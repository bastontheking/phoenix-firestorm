/**
 * @file fsfloateraichatbot.h
 * @brief ChatBot tab in the conversations window, backed by the user's own LLM
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

#ifndef FS_FLOATERAICHATBOT_H
#define FS_FLOATERAICHATBOT_H

#include "llfloater.h"

class LLButton;
class LLComboBox;
class LLLineEditor;
class LLTextBox;
class LLTextEditor;

/**
 * A private conversation with the user's LLM, shown as a tab right after
 * Nearby Chat. Nothing typed here goes to Second Life.
 *
 * The conversation is kept per account in ai_chatbot_history.xml and the
 * last FSAIChatbotHistoryLength entries are sent as context. The send
 * button can be switched to "Send w/o history": a one-off question that
 * neither reads nor changes the conversation.
 */
class FSFloaterAIChatbot : public LLFloater
{
public:
    FSFloaterAIChatbot(const LLSD& key);
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

    void onSend();
    void onClear();
    void onSendModeChanged();
    void continueWithWebSearch(const std::string& question, const std::string& profiles, bool one_off);
    void askModel(const std::string& question, const std::string& web_context, const std::string& sources, const std::string& profiles, bool one_off);
    void appendEntry(const Entry& entry, bool one_off = false);
    void appendLine(const std::string& text, const LLColor4& color, bool italic = false);
    void setBusy(bool busy, const std::string& status = LLStringUtil::null);
    void loadHistory();
    void saveHistory() const;
    static std::string historyFile();

    std::vector<Entry> mHistory;
    bool               mBusy = false;

    LLTextEditor*   mChatHistory = nullptr;
    LLLineEditor*   mInput = nullptr;
    LLButton*       mSendBtn = nullptr;
    LLComboBox*     mSendMode = nullptr;
    LLTextBox*      mStatusText = nullptr;
};

#endif // FS_FLOATERAICHATBOT_H
