/**
 * @file fsaichatassist.h
 * @brief Inline AI writing suggestions in the chat bar (nearby chat and IM)
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

#ifndef FS_AICHATASSIST_H
#define FS_AICHATASSIST_H

#include <memory>
#include <string>

class LLButton;
class LLComboBox;
class LLLayoutPanel;
class LLPanel;
class LLScrollListCtrl;
class LLTextBox;
class LLTextEditor;

/**
 * Wires the AI controls of a chat floater:
 *
 *   [ suggestions strip (hidden until used)            ] [again] [x]
 *   [input] [language] [style] [settings] [emoji] [send]
 *
 * Picking a style asks the user's LLM for rewrites of the text in the input,
 * written in the selected language. Clicking a suggestion puts it in the
 * input; nothing is ever sent automatically.
 *
 * All widgets are looked up with findChild, so skins without them simply get
 * no AI controls.
 */
class FSAIChatAssist
{
public:
    FSAIChatAssist(LLPanel* owner, LLTextEditor* input);
    ~FSAIChatAssist();

private:
    void request(bool more_creative);
    void onSuggestionPicked();
    void showStrip(bool show);
    void setStatus(const std::string& status);

    LLTextEditor*     mInput = nullptr;
    LLComboBox*       mLanguageCombo = nullptr;
    LLComboBox*       mStyleCombo = nullptr;
    LLLayoutPanel*    mStripPanel = nullptr;
    LLScrollListCtrl* mSuggestionsList = nullptr;
    LLTextBox*        mStatusText = nullptr;

    unsigned int      mRequestId = 0;
    std::string       mRequestedText;
    std::shared_ptr<bool> mAlive; // callbacks hold a weak_ptr to detect destruction
};

#endif // FS_AICHATASSIST_H
