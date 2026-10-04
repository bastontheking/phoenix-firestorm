/**
 * @file fsfloateraichatbot.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsfloateraichatbot.h"

#include "fsaiwriter.h"
#include "fsnearbychathub.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "lldir.h"
#include "llfloaterreg.h"
#include "lllineeditor.h"
#include "llsdserialize.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llviewerchat.h"

#include <fstream>

namespace
{
    constexpr size_t MAX_STORED_ENTRIES = 200;
    constexpr size_t MAX_CHAT_BYTES = 1000; // nearby chat limit is 1023 bytes per message
    constexpr size_t MAX_CHAT_PARTS = 4;

    // Markdown emphasis is noise in a plain text box.
    std::string tidyReply(std::string text)
    {
        LLStringUtil::replaceString(text, "**", "");
        LLStringUtil::replaceString(text, "__", "");
        LLStringUtil::trim(text);
        return text;
    }

    // Split text into chat-sized parts at word boundaries.
    std::vector<std::string> splitForChat(const std::string& text)
    {
        std::vector<std::string> parts;
        std::string remaining = text;
        LLStringUtil::replaceString(remaining, "\n", " ");
        LLStringUtil::trim(remaining);
        while (!remaining.empty() && parts.size() < MAX_CHAT_PARTS)
        {
            if (remaining.size() <= MAX_CHAT_BYTES)
            {
                parts.push_back(remaining);
                remaining.clear();
                break;
            }
            size_t cut = remaining.rfind(' ', MAX_CHAT_BYTES);
            if (cut == std::string::npos || cut < MAX_CHAT_BYTES / 2)
            {
                cut = MAX_CHAT_BYTES;
                // do not cut inside a UTF-8 sequence
                while (cut > 0 && (remaining[cut] & 0xC0) == 0x80)
                {
                    --cut;
                }
            }
            parts.push_back(remaining.substr(0, cut));
            remaining = remaining.substr(cut);
            LLStringUtil::trim(remaining);
        }
        if (!remaining.empty() && !parts.empty())
        {
            parts.back() += " ...";
        }
        return parts;
    }
}

FSFloaterAIChatbot::FSFloaterAIChatbot(const LLSD& key)
:   LLFloater(key)
{
}

bool FSFloaterAIChatbot::postBuild()
{
    mChatHistory = getChild<LLTextEditor>("chatbot_history");
    mInput = getChild<LLLineEditor>("chatbot_input");
    mSendBtn = getChild<LLButton>("chatbot_send_btn");
    mOneOffCheck = getChild<LLCheckBoxCtrl>("chatbot_oneoff_check");
    mWebSearchCheck = getChild<LLCheckBoxCtrl>("chatbot_websearch_check");
    mStatusText = getChild<LLTextBox>("chatbot_status");

    mChatHistory->setReadOnly(true);
    mInput->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSend(); });
    mInput->setCommitOnFocusLost(false);
    mSendBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onSend(); });
    getChild<LLButton>("chatbot_to_chat_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onSendToChat(); });
    getChild<LLButton>("chatbot_clear_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onClear(); });
    getChild<LLButton>("chatbot_settings_btn")->setClickedCallback([](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance("fs_ai_writer"); });

    loadHistory();
    setBusy(false);
    return true;
}

std::string FSFloaterAIChatbot::historyFile() const
{
    const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "");
    if (dir.empty())
    {
        return std::string(); // not logged in yet
    }
    return gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "ai_chatbot_history.xml");
}

void FSFloaterAIChatbot::loadHistory()
{
    mHistory.clear();
    const std::string file = historyFile();
    if (!file.empty())
    {
        llifstream in(file.c_str());
        LLSD data;
        if (in.is_open() && LLSDSerialize::fromXML(data, in) > 0 && data.isArray())
        {
            for (const LLSD& item : llsd::inArray(data))
            {
                mHistory.push_back({ item["role"].asString(), item["content"].asString(), item["oneoff"].asBoolean() });
            }
        }
    }

    mChatHistory->clear();
    if (mHistory.empty())
    {
        appendLine(getString("welcome"), LLColor4::grey3, true);
    }
    for (const Entry& entry : mHistory)
    {
        appendEntry(entry);
    }
}

void FSFloaterAIChatbot::saveHistory() const
{
    const std::string file = historyFile();
    if (file.empty())
    {
        return;
    }
    LLSD data = LLSD::emptyArray();
    const size_t first = mHistory.size() > MAX_STORED_ENTRIES ? mHistory.size() - MAX_STORED_ENTRIES : 0;
    for (size_t i = first; i < mHistory.size(); ++i)
    {
        LLSD item;
        item["role"] = mHistory[i].mRole;
        item["content"] = mHistory[i].mContent;
        item["oneoff"] = mHistory[i].mOneOff;
        data.append(item);
    }
    llofstream out(file.c_str());
    if (out.is_open())
    {
        LLSDSerialize::toPrettyXML(data, out);
    }
}

void FSFloaterAIChatbot::appendLine(const std::string& text, const LLColor4& color, bool italic)
{
    LLFontGL* fontp = LLViewerChat::getChatFont();
    LLStyle::Params style;
    style.color(color);
    style.readonly_color(color);
    style.font.name(LLFontGL::nameFromFont(fontp));
    style.font.size(LLFontGL::sizeFromFont(fontp));
    if (italic)
    {
        style.font.style("ITALIC");
    }
    mChatHistory->appendText(text, mChatHistory->getLength() > 0, style);
    mChatHistory->setCursorAndScrollToEnd();
}

void FSFloaterAIChatbot::appendEntry(const Entry& entry)
{
    static LLCachedControl<LLColor4> bot_color(gSavedSettings, "FSAIWriterTranslatorColor", LLColor4(0.55f, 0.9f, 0.6f, 1.f));
    if (entry.mRole == "user")
    {
        std::string prefix = getString("you") + ": ";
        if (entry.mOneOff)
        {
            prefix = getString("you_oneoff") + ": ";
        }
        appendLine(prefix + entry.mContent, LLColor4::white);
    }
    else
    {
        appendLine(getString("bot") + ": " + entry.mContent, (LLColor4)bot_color);
    }
}

void FSFloaterAIChatbot::setBusy(bool busy, const std::string& status)
{
    mBusy = busy;
    mSendBtn->setEnabled(!busy);
    mStatusText->setText(status);
}

void FSFloaterAIChatbot::onSend()
{
    if (mBusy)
    {
        return;
    }
    std::string question = mInput->getText();
    LLStringUtil::trim(question);
    if (question.empty())
    {
        return;
    }
    mInput->clear();

    const bool one_off = mOneOffCheck->get();
    Entry user_entry{ "user", question, one_off };
    mHistory.push_back(user_entry);
    appendEntry(user_entry);
    saveHistory();

    if (!mWebSearchCheck->get())
    {
        askModel(question, LLStringUtil::null, one_off, LLStringUtil::null);
        return;
    }

    setBusy(true, getString("searching"));
    LLHandle<LLFloater> handle = getHandle();
    FSAIWriter::webSearch(question, [handle, question, one_off](bool success, const std::vector<FSAIWriter::SearchResult>& results, const std::string& error)
    {
        FSFloaterAIChatbot* self = static_cast<FSFloaterAIChatbot*>(handle.get());
        if (!self)
        {
            return;
        }
        std::string web_context;
        std::string sources;
        if (success)
        {
            for (size_t i = 0; i < results.size(); ++i)
            {
                web_context += llformat("[%d] %s\n%s\n%s\n\n", (S32)i + 1, results[i].mTitle.c_str(), results[i].mUrl.c_str(), results[i].mSnippet.c_str());
                sources += llformat("[%d] %s - %s\n", (S32)i + 1, results[i].mTitle.c_str(), results[i].mUrl.c_str());
            }
        }
        else
        {
            self->appendLine(error, LLColor4::grey3, true);
        }
        self->askModel(question, web_context, one_off, sources);
    });
}

void FSFloaterAIChatbot::askModel(const std::string& question, const std::string& web_context, bool one_off, const std::string& sources)
{
    static LLCachedControl<U32> history_length(gSavedSettings, "FSAIChatbotHistoryLength", 30);

    std::vector<FSAIWriter::ChatMessage> messages;
    messages.push_back({ "system",
        "You are a friendly, helpful assistant built into the Firestorm viewer for Second Life. "
        "Answer in the same language the user writes in. Be clear and concise; use short paragraphs "
        "and plain text (no markdown tables)." });

    // Conversation context: the latest entries, skipping one-off exchanges
    // and the question we are about to add ourselves.
    if (!one_off)
    {
        std::vector<FSAIWriter::ChatMessage> context;
        for (size_t i = mHistory.size() - 1; i-- > 0 && context.size() < (size_t)(U32)history_length; )
        {
            if (!mHistory[i].mOneOff)
            {
                context.push_back({ mHistory[i].mRole, mHistory[i].mContent });
            }
        }
        messages.insert(messages.end(), context.rbegin(), context.rend());
    }

    std::string prompt = question;
    if (!web_context.empty())
    {
        prompt = "Web search results for the question below:\n\n" + web_context +
                 "Using these results where relevant (cite them as [1], [2], ...), answer:\n" + question;
    }
    messages.push_back({ "user", prompt });

    setBusy(true, getString("thinking"));
    LLHandle<LLFloater> handle = getHandle();
    FSAIWriter::chat(messages, [handle, one_off, sources](bool success, const std::string& reply, const std::string& error)
    {
        FSFloaterAIChatbot* self = static_cast<FSFloaterAIChatbot*>(handle.get());
        if (!self)
        {
            return;
        }
        self->setBusy(false);
        if (!success)
        {
            self->appendLine(error, LLColor4::red2, true);
            return;
        }
        Entry bot_entry{ "assistant", tidyReply(reply), one_off };
        self->mHistory.push_back(bot_entry);
        self->appendEntry(bot_entry);
        if (!sources.empty())
        {
            self->appendLine(self->getString("sources") + "\n" + sources, LLColor4::grey3, true);
        }
        self->saveHistory();
    });
}

void FSFloaterAIChatbot::onSendToChat()
{
    for (auto it = mHistory.rbegin(); it != mHistory.rend(); ++it)
    {
        if (it->mRole == "assistant" && !it->mContent.empty())
        {
            // Explicit user action: post the last answer to nearby chat,
            // split into chat-sized messages if needed.
            for (const std::string& part : splitForChat(it->mContent))
            {
                FSNearbyChat::instance().sendChatFromViewer(part, CHAT_TYPE_NORMAL, false);
            }
            mStatusText->setText(getString("sent_to_chat"));
            return;
        }
    }
    mStatusText->setText(getString("nothing_to_send"));
}

void FSFloaterAIChatbot::onClear()
{
    mHistory.clear();
    saveHistory();
    mChatHistory->clear();
    appendLine(getString("welcome"), LLColor4::grey3, true);
    setBusy(false, getString("cleared"));
}
