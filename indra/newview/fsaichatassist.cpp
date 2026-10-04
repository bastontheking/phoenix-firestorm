/**
 * @file fsaichatassist.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsaichatassist.h"

#include "fsaiwriter.h"
#include "llbutton.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "lllayoutstack.h"
#include "llpanel.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "lltrans.h"

FSAIChatAssist::FSAIChatAssist(LLPanel* owner, LLTextEditor* input)
:   mInput(input),
    mAlive(std::make_shared<bool>(true))
{
    if (!owner || !input)
    {
        return;
    }

    mLanguageCombo = owner->findChild<LLComboBox>("ai_language_combo");
    mStyleCombo = owner->findChild<LLComboBox>("ai_style_combo");
    mStripPanel = owner->findChild<LLLayoutPanel>("ai_suggestions_layout_panel");
    mSuggestionsList = owner->findChild<LLScrollListCtrl>("ai_suggestions_list");
    mStatusText = owner->findChild<LLTextBox>("ai_status_text");

    if (mStyleCombo)
    {
        // Choosing a style is the action: it asks for suggestions right away.
        mStyleCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { request(false); });
    }
    if (mLanguageCombo)
    {
        // Switching language while suggestions are shown refreshes them.
        mLanguageCombo->setCommitCallback([this](LLUICtrl*, const LLSD&)
        {
            if (mStripPanel && mStripPanel->getVisible())
            {
                request(false);
            }
        });
    }
    if (mSuggestionsList)
    {
        mSuggestionsList->setCommitOnSelectionChange(true);
        mSuggestionsList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSuggestionPicked(); });
    }
    if (LLButton* btn = owner->findChild<LLButton>("ai_settings_btn"))
    {
        btn->setClickedCallback([](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance("fs_ai_writer"); });
    }
    if (LLButton* btn = owner->findChild<LLButton>("ai_retry_btn"))
    {
        btn->setClickedCallback([this](LLUICtrl*, const LLSD&) { request(true); });
    }
    if (LLButton* btn = owner->findChild<LLButton>("ai_close_btn"))
    {
        btn->setClickedCallback([this](LLUICtrl*, const LLSD&) { showStrip(false); });
    }

    showStrip(false);
}

FSAIChatAssist::~FSAIChatAssist()
{
    mAlive.reset();
}

void FSAIChatAssist::onMessageSent()
{
    showStrip(false);
    mRequestedText.clear();
}

void FSAIChatAssist::showStrip(bool show)
{
    if (mStripPanel)
    {
        mStripPanel->setVisible(show);
    }
    if (!show)
    {
        ++mRequestId; // drop any answer still on its way
        if (mSuggestionsList)
        {
            mSuggestionsList->deleteAllItems();
        }
    }
}

void FSAIChatAssist::setStatus(const std::string& status)
{
    if (mStatusText)
    {
        mStatusText->setText(status);
    }
}

void FSAIChatAssist::request(bool more_creative)
{
    if (!mInput || !mSuggestionsList)
    {
        return;
    }

    std::string text = mInput->getText();
    LLStringUtil::trim(text);
    // "Again" keeps working on the original text even after a suggestion
    // was put in the input.
    if (more_creative && !mRequestedText.empty())
    {
        text = mRequestedText;
    }

    showStrip(true);
    mSuggestionsList->deleteAllItems();
    if (text.empty())
    {
        setStatus(LLTrans::getString("AIWriterTypeFirst"));
        return;
    }

    mRequestedText = text;
    const unsigned int request_id = ++mRequestId;
    setStatus(LLTrans::getString("AIWriterThinking"));

    const std::string style = mStyleCombo ? mStyleCombo->getValue().asString() : "nicer";
    const std::string language = mLanguageCombo ? mLanguageCombo->getValue().asString() : "pt";
    std::weak_ptr<bool> alive = mAlive;

    FSAIWriter::rewrite(text, style, language, more_creative, [this, alive, request_id](const FSAIWriter::Result& result)
    {
        if (alive.expired() || request_id != mRequestId)
        {
            return; // floater closed, strip dismissed, or a newer request
        }
        if (!result.mSuccess)
        {
            setStatus(result.mError);
            return;
        }
        for (const std::string& suggestion : result.mSuggestions)
        {
            LLSD row;
            row["value"] = suggestion;
            row["columns"][0]["column"] = "text";
            row["columns"][0]["value"] = suggestion;
            mSuggestionsList->addElement(row);
        }
        setStatus(LLTrans::getString("AIWriterPickOne"));
    });
}

void FSAIChatAssist::onSuggestionPicked()
{
    LLScrollListItem* item = mSuggestionsList ? mSuggestionsList->getFirstSelected() : nullptr;
    if (!item || !mInput)
    {
        return;
    }
    // Put it in the input only; the user still decides whether to send.
    mInput->setText(item->getValue().asString());
    mInput->endOfDoc();
    mInput->setFocus(true);
    mSuggestionsList->deselectAllItems();
}
