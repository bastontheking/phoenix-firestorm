/**
 * @file fsfloateraiwriter.cpp
 * @brief Settings floater for the AI chat assistant (server, model, translation)
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
#include "fsfloateraichatbot.h"
#include "llbutton.h"
#include "llcombobox.h"
#include "lltextbox.h"
#include "llviewercontrol.h"

FSFloaterAIWriter::FSFloaterAIWriter(const LLSD& key)
:   LLFloater(key)
{
}

bool FSFloaterAIWriter::postBuild()
{
    mModelCombo = getChild<LLComboBox>("model_combo");
    mStatusText = getChild<LLTextBox>("status_text");

    getChild<LLButton>("fetch_models_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onFetchModels(); });
    getChild<LLButton>("test_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onTest(); });
    getChild<LLButton>("close_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    getChild<LLButton>("chatbot_clear_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&)
    {
        FSFloaterAIChatbot::clearSavedHistory();
        setStatus(getString("history_cleared"));
    });

    mModelCombo->setTextEntry(gSavedSettings.getString("FSAIWriterModel"));
    auto save_model = [this](LLUICtrl*, const LLSD&)
    {
        gSavedSettings.setString("FSAIWriterModel", mModelCombo->getSimple());
    };
    mModelCombo->setCommitCallback(save_model);
    mModelCombo->setTextEntryCallback(save_model);
    return true;
}

void FSFloaterAIWriter::setStatus(const std::string& status)
{
    mStatusText->setText(status);
}

void FSFloaterAIWriter::onFetchModels()
{
    setStatus(getString("fetching_models"));
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
            self->setStatus(error);
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
        self->setStatus(llformat("%d model(s) found.", (S32)models.size()));
    });
}

void FSFloaterAIWriter::onTest()
{
    setStatus(getString("testing"));
    LLHandle<LLFloater> handle = getHandle();
    FSAIWriter::translate("Hello! How are you today?", false, [handle](bool success, const std::string& translation, const std::string& error)
    {
        FSFloaterAIWriter* self = static_cast<FSFloaterAIWriter*>(handle.get());
        if (self)
        {
            self->setStatus(success ? "OK: \"Hello! How are you today?\" -> \"" + translation + "\"" : error);
        }
    });
}
