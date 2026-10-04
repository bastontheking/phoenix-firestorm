/**
 * @file fsfloateraiwriter.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsfloateraiwriter.h"

#include "fsaiwriter.h"
#include "llbutton.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "llviewercontrol.h"

FSFloaterAIWriter::FSFloaterAIWriter(const LLSD& key)
:   LLFloater(key)
{
}

bool FSFloaterAIWriter::postBuild()
{
    mStyleCombo = getChild<LLComboBox>("style_combo");
    mOriginalText = getChild<LLTextEditor>("original_text");
    mSuggestionsList = getChild<LLScrollListCtrl>("suggestions_list");
    mResultText = getChild<LLTextEditor>("result_text");
    mStatusText = getChild<LLTextBox>("status_text");
    mUseBtn = getChild<LLButton>("use_btn");
    mRetryBtn = getChild<LLButton>("retry_btn");
    mModelCombo = getChild<LLComboBox>("model_combo");

    mOriginalText->setReadOnly(true);

    // Changing the style asks again right away.
    mStyleCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { requestSuggestions(false); });
    mSuggestionsList->setCommitOnSelectionChange(true);
    mSuggestionsList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSuggestionSelected(); });
    mSuggestionsList->setDoubleClickCallback([this]() { onSuggestionSelected(); onUse(); });
    mUseBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onUse(); });
    mRetryBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { requestSuggestions(true); });
    getChild<LLButton>("close_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    getChild<LLButton>("fetch_models_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onFetchModels(); });

    mModelCombo->setTextEntry(gSavedSettings.getString("FSAIWriterModel"));
    auto save_model = [this](LLUICtrl*, const LLSD&)
    {
        gSavedSettings.setString("FSAIWriterModel", mModelCombo->getSimple());
    };
    mModelCombo->setCommitCallback(save_model);
    mModelCombo->setTextEntryCallback(save_model);

    mUseBtn->setEnabled(false);
    return true;
}

// static
void FSFloaterAIWriter::showFor(LLTextEditor* chat_input)
{
    if (!chat_input)
    {
        return;
    }
    FSFloaterAIWriter* floater = LLFloaterReg::showTypedInstance<FSFloaterAIWriter>("fs_ai_writer");
    if (floater)
    {
        floater->setTarget(chat_input);
        floater->requestSuggestions(false);
    }
}

void FSFloaterAIWriter::setTarget(LLTextEditor* chat_input)
{
    mTarget = chat_input->getHandle();
    mOriginal = chat_input->getText();
    LLStringUtil::trim(mOriginal);
    mOriginalText->setText(mOriginal);
    mSuggestionsList->deleteAllItems();
    mResultText->setText(LLStringUtil::null);
    mUseBtn->setEnabled(false);
}

void FSFloaterAIWriter::setBusy(bool busy, const std::string& status)
{
    mStatusText->setText(status);
    mRetryBtn->setEnabled(!busy && !mOriginal.empty());
    mStyleCombo->setEnabled(!busy);
}

void FSFloaterAIWriter::requestSuggestions(bool more_creative)
{
    if (mOriginal.empty())
    {
        setBusy(false, getString("empty_text"));
        return;
    }

    const U32 request_id = ++mRequestId;
    setBusy(true, getString("thinking"));
    mSuggestionsList->deleteAllItems();

    LLHandle<LLFloater> handle = getHandle();
    const std::string style = mStyleCombo->getValue().asString();
    FSAIWriter::rewrite(mOriginal, style, more_creative, [handle, request_id](const FSAIWriter::Result& result)
    {
        FSFloaterAIWriter* self = static_cast<FSFloaterAIWriter*>(handle.get());
        if (!self || request_id != self->mRequestId)
        {
            return; // floater closed, or a newer request replaced this one
        }
        if (!result.mSuccess)
        {
            self->setBusy(false, result.mError);
            return;
        }
        for (const std::string& suggestion : result.mSuggestions)
        {
            // The row must use the column declared in the XUI ("text"),
            // otherwise the cell goes to an unnamed, zero-width column.
            LLSD row;
            row["value"] = suggestion;
            row["columns"][0]["column"] = "text";
            row["columns"][0]["value"] = suggestion;
            self->mSuggestionsList->addElement(row);
        }
        self->mSuggestionsList->selectFirstItem();
        self->onSuggestionSelected();
        self->setBusy(false, self->getString("ready"));
    });
}

void FSFloaterAIWriter::onSuggestionSelected()
{
    LLScrollListItem* item = mSuggestionsList->getFirstSelected();
    if (!item)
    {
        return;
    }
    mResultText->setText(item->getValue().asString());
    mUseBtn->setEnabled(true);
}

void FSFloaterAIWriter::onUse()
{
    LLTextEditor* target = dynamic_cast<LLTextEditor*>(mTarget.get());
    const std::string text = mResultText->getText();
    if (!target || text.empty())
    {
        setBusy(false, getString("no_target"));
        return;
    }
    // Only replace the text in the input box; the user still decides
    // whether and when to send it.
    target->setText(text);
    target->endOfDoc();
    target->setFocus(true);
    closeFloater();
}

void FSFloaterAIWriter::onFetchModels()
{
    setBusy(true, getString("fetching_models"));
    LLHandle<LLFloater> handle = getHandle();
    FSAIWriter::fetchModels([handle](bool success, const std::vector<std::string>& models, const std::string& error)
    {
        FSFloaterAIWriter* self = static_cast<FSFloaterAIWriter*>(handle.get());
        if (!self)
        {
            return;
        }
        if (!success)
        {
            self->setBusy(false, error);
            return;
        }
        const std::string current = gSavedSettings.getString("FSAIWriterModel");
        self->mModelCombo->removeall();
        for (const std::string& model : models)
        {
            self->mModelCombo->add(model, LLSD(model));
        }
        if (current.empty() || !self->mModelCombo->selectByValue(LLSD(current)))
        {
            self->mModelCombo->selectFirstItem();
            gSavedSettings.setString("FSAIWriterModel", self->mModelCombo->getSimple());
        }
        self->setBusy(false, llformat("%d model(s) found.", (S32)models.size()));
    });
}
