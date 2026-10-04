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
#include "lleventtimer.h"
#include "lltrans.h"
#include "llviewercontrol.h"

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

    // Changing style or language refreshes the suggestions right away.
    auto refresh = [this](LLUICtrl*, const LLSD&)
    {
        mRequestedText.clear();
        mPickedText.clear();
        poll();
        if (!mLastSeenText.empty())
        {
            request(false);
        }
    };
    if (mStyleCombo)
    {
        mStyleCombo->setCommitCallback(refresh);
    }
    if (mLanguageCombo)
    {
        mLanguageCombo->setCommitCallback(refresh);
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
    showStrip(false);

    if (mSuggestionsList)
    {
        // Cheap check of the input a few times per second; the AI is only
        // asked once the text has stopped changing for a moment.
        mPollTimer = LLEventTimer::run_every(0.25f, [this]() { poll(); });
    }
}

FSAIChatAssist::~FSAIChatAssist()
{
    // run_every() timers are never deleted by LLEventTimer itself
    delete mPollTimer;
    mAlive.reset();
}

void FSAIChatAssist::onMessageSent()
{
    showStrip(false);
    mRequestedText.clear();
    mPickedText.clear();
}

void FSAIChatAssist::poll()
{
    static LLCachedControl<bool> auto_suggest(gSavedSettings, "FSAIWriterAutoSuggest", true);
    static LLCachedControl<F32> delay(gSavedSettings, "FSAIWriterAutoSuggestDelay", 1.0f);
    if (!mInput)
    {
        return;
    }

    std::string text = mInput->getText();
    LLStringUtil::trim(text);
    if (text != mLastSeenText)
    {
        mLastSeenText = text;
        mSinceChange.reset();
    }

    // Nothing typed (or feature off, or chat window hidden): no strip.
    if (text.empty() || !auto_suggest || !mInput->isInVisibleChain())
    {
        if (mStripPanel && mStripPanel->getVisible())
        {
            showStrip(false);
        }
        mRequestedText.clear();
        mPickedText.clear();
        return;
    }

    // Already handled, the user picked a suggestion, chat commands, or not
    // enough words yet.
    if (text == mRequestedText || text == mPickedText || text[0] == '/')
    {
        return;
    }
    S32 letters = 0;
    for (const llwchar c : utf8str_to_wstring(text))
    {
        letters += iswalpha((wint_t)c) ? 1 : 0;
    }
    if (letters < 2 || mSinceChange.getElapsedTimeF32() < (F32)delay)
    {
        return;
    }
    request(false);
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

    if (text.empty())
    {
        showStrip(false);
        return;
    }
    showStrip(true);

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
        mSuggestionsList->deleteAllItems();
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
    mPickedText = item->getValue().asString();
    LLStringUtil::trim(mPickedText);
    mInput->setText(item->getValue().asString());
    mInput->endOfDoc();
    mInput->setFocus(true);
    mSuggestionsList->deselectAllItems();
}
