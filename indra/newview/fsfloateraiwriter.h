/**
 * @file fsfloateraiwriter.h
 * @brief Floater showing AI suggestions for the message being typed in chat
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

#ifndef FS_FLOATERAIWRITER_H
#define FS_FLOATERAIWRITER_H

#include "llfloater.h"
#include "llhandle.h"

class LLButton;
class LLComboBox;
class LLScrollListCtrl;
class LLTextBox;
class LLTextEditor;

class FSFloaterAIWriter : public LLFloater
{
public:
    FSFloaterAIWriter(const LLSD& key);
    ~FSFloaterAIWriter() override = default;

    bool postBuild() override;

    // Open the floater for the given chat input and immediately ask the
    // AI for suggestions on its current text.
    static void showFor(LLTextEditor* chat_input);

private:
    void setTarget(LLTextEditor* chat_input);
    void requestSuggestions(bool more_creative);
    void onSuggestionSelected();
    void onUse();
    void onFetchModels();
    void setBusy(bool busy, const std::string& status);

    LLHandle<LLView>  mTarget;
    std::string       mOriginal;
    U32               mRequestId = 0;

    LLComboBox*       mStyleCombo = nullptr;
    LLTextEditor*     mOriginalText = nullptr;
    LLScrollListCtrl* mSuggestionsList = nullptr;
    LLTextEditor*     mResultText = nullptr;
    LLTextBox*        mStatusText = nullptr;
    LLButton*         mUseBtn = nullptr;
    LLButton*         mRetryBtn = nullptr;
    LLComboBox*       mModelCombo = nullptr;
};

#endif // FS_FLOATERAIWRITER_H
