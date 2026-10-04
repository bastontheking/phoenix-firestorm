/**
 * @file fsaiwriter.cpp
 * @brief Chat writing assistant backed by a user-configured, OpenAI-compatible LLM endpoint
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

#include "fsaiwriter.h"

#include "llcorehttputil.h"
#include "llcoros.h"
#include "llhttpconstants.h"
#include "llviewercontrol.h"

#include "lluri.h"

#include <boost/json.hpp>
#include <boost/regex.hpp>

namespace
{
    constexpr size_t MAX_SUGGESTIONS = 3;
    const std::string SAME_MARKER = "##SAME##";

    std::string trim(const std::string& in)
    {
        std::string s = in;
        LLStringUtil::trim(s);
        return s;
    }

    // Read the assistant text from an OpenAI-style response:
    // { "choices": [ { "message": { "content": "..." } } ] }
    // (also accepts the legacy completions "text" field)
    bool extractContent(const boost::json::value& root, std::string& content, std::string& error)
    {
        if (!root.is_object())
        {
            error = "Unexpected response from the server";
            return false;
        }
        const boost::json::object& obj = root.as_object();
        if (const boost::json::value* err = obj.if_contains("error"))
        {
            if (err->is_object() && err->as_object().if_contains("message") && err->as_object().at("message").is_string())
            {
                error = std::string(err->as_object().at("message").as_string());
            }
            else if (err->is_string())
            {
                error = std::string(err->as_string());
            }
            else
            {
                error = "The server returned an error";
            }
            return false;
        }
        const boost::json::value* choices = obj.if_contains("choices");
        if (!choices || !choices->is_array() || choices->as_array().empty() || !choices->as_array()[0].is_object())
        {
            error = "The server response has no choices";
            return false;
        }
        const boost::json::object& choice = choices->as_array()[0].as_object();
        if (const boost::json::value* message = choice.if_contains("message"))
        {
            if (message->is_object())
            {
                const boost::json::value* c = message->as_object().if_contains("content");
                if (c && c->is_string())
                {
                    content = std::string(c->as_string());
                    const boost::json::value* reasoning = message->as_object().if_contains("reasoning_content");
                    const boost::json::value* finish = choice.if_contains("finish_reason");
                    if (trim(content).empty() && reasoning && reasoning->is_string() && !reasoning->as_string().empty()
                        && finish && finish->is_string() && finish->as_string() == "length")
                    {
                        error = "The model used its whole answer budget for reasoning. Enable FSAIWriterDisableThinking or start the server with reasoning disabled.";
                        return false;
                    }
                    return true;
                }
            }
        }
        if (const boost::json::value* text = choice.if_contains("text"))
        {
            if (text->is_string())
            {
                content = std::string(text->as_string());
                return true;
            }
        }
        error = "The server response has no text";
        return false;
    }

    LLCore::HttpHeaders::ptr_t makeHeaders()
    {
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, HTTP_CONTENT_JSON);
        headers->append(HTTP_OUT_HEADER_ACCEPT, HTTP_CONTENT_JSON);
        const std::string api_key = trim(gSavedSettings.getString("FSAIWriterApiKey"));
        if (!api_key.empty())
        {
            headers->append("Authorization", "Bearer " + api_key);
        }
        return headers;
    }

    LLCore::HttpOptions::ptr_t makeOptions()
    {
        LLCore::HttpOptions::ptr_t options = std::make_shared<LLCore::HttpOptions>();
        const U32 timeout = llclamp(gSavedSettings.getU32("FSAIWriterTimeout"), 5U, 600U);
        options->setTimeout(timeout);
        options->setTransferTimeout(timeout);
        options->setRetries(0);
        return options;
    }

    std::string describeFailure(const LLSD& result)
    {
        const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        std::string msg = status.toString();
        if (status == LLCore::HttpStatus(LLCore::HttpStatus::EXT_CURL_EASY, CURLE_COULDNT_CONNECT))
        {
            msg = "Could not connect to the AI server. Check that it is running and that the address is correct.";
        }
        else if (status == LLCore::HttpStatus(LLCore::HttpStatus::EXT_CURL_EASY, CURLE_OPERATION_TIMEDOUT))
        {
            msg = "The AI server took too long to answer (FSAIWriterTimeout).";
        }
        return msg;
    }
}

// static
const std::vector<std::string>& FSAIWriter::getStyleKeys()
{
    static const std::vector<std::string> keys{ "nicer", "formal", "casual", "romantic", "funny", "fix" };
    return keys;
}

// static
std::string FSAIWriter::getStyleInstruction(const std::string& style)
{
    if (style == "formal")   return "Rewrite it in a polite, formal and well-written way.";
    if (style == "casual")   return "Rewrite it in a relaxed, friendly and natural chat tone.";
    if (style == "romantic") return "Rewrite it in a sweet, charming and romantic way, without being over the top.";
    if (style == "funny")    return "Rewrite it in a witty, playful and funny way.";
    if (style == "fix")      return "Only fix spelling, grammar, accents and punctuation. Keep the wording and tone as close to the original as possible.";
    return "Rewrite it so it reads more beautifully, clearly and naturally, with correct spelling and grammar.";
}

// static
std::string FSAIWriter::getModelsUrl(const std::string& chat_url)
{
    std::string url = trim(chat_url);
    const std::string suffix = "/chat/completions";
    if (url.size() >= suffix.size() && url.compare(url.size() - suffix.size(), suffix.size(), suffix) == 0)
    {
        return url.substr(0, url.size() - suffix.size()) + "/models";
    }
    while (!url.empty() && url.back() == '/')
    {
        url.pop_back();
    }
    return url + "/models";
}

// static
std::vector<std::string> FSAIWriter::parseSuggestions(const std::string& raw_content)
{
    std::string content = raw_content;

    // Reasoning models (DeepSeek-R1, Qwen3, Gemma 4, ...) may prepend their
    // thinking, or an empty thought channel, to the answer.
    for (const char* tag : { "</think>", "</thinking>", "<channel|>" })
    {
        const size_t end = content.rfind(tag);
        if (end != std::string::npos)
        {
            content = content.substr(end + strlen(tag));
        }
    }

    std::vector<std::string> out;
    std::istringstream lines(content);
    std::string line;
    while (std::getline(lines, line) && out.size() < MAX_SUGGESTIONS)
    {
        line = trim(line);
        if (line.empty())
        {
            continue;
        }
        // Drop list markers: "1." "2)" "-" "*" "•"
        size_t pos = 0;
        while (pos < line.size() && isdigit((unsigned char)line[pos]))
        {
            ++pos;
        }
        if (pos > 0 && pos < line.size() && (line[pos] == '.' || line[pos] == ')' || line[pos] == ':'))
        {
            line = trim(line.substr(pos + 1));
        }
        else if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0)
        {
            line = trim(line.substr(2));
        }
        else if (line.rfind("\xE2\x80\xA2", 0) == 0) // bullet
        {
            line = trim(line.substr(3));
        }
        // Drop surrounding quotes (ASCII or typographic)
        if (line.size() >= 2 && line.front() == '"' && line.back() == '"')
        {
            line = trim(line.substr(1, line.size() - 2));
        }
        if (line.size() >= 6 && line.rfind("\xE2\x80\x9C", 0) == 0 && line.compare(line.size() - 3, 3, "\xE2\x80\x9D") == 0)
        {
            line = trim(line.substr(3, line.size() - 6));
        }
        // Skip chatter such as "Here are three options:"
        if (!line.empty() && line.back() == ':' && out.empty())
        {
            continue;
        }
        if (!line.empty())
        {
            out.push_back(line);
        }
    }

    if (out.empty())
    {
        const std::string whole = trim(content);
        if (!whole.empty())
        {
            out.push_back(whole);
        }
    }
    return out;
}

// static
void FSAIWriter::rewrite(const std::string& text, const std::string& style, const std::string& language, bool more_creative, rewrite_callback_t callback)
{
    LLCoros::instance().launch("FSAIWriter::rewriteCoro",
        [text, style, language, more_creative, callback]() { rewriteCoro(text, style, language, more_creative, callback); });
}

// static
std::string FSAIWriter::stripReasoning(const std::string& raw_content)
{
    std::string content = raw_content;
    // Reasoning models (DeepSeek-R1, Qwen3, Gemma 4, ...) may prepend their
    // thinking, or an empty thought channel, to the answer.
    for (const char* tag : { "</think>", "</thinking>", "<channel|>" })
    {
        const size_t end = content.rfind(tag);
        if (end != std::string::npos)
        {
            content = content.substr(end + strlen(tag));
        }
    }
    return trim(content);
}

// static
bool FSAIWriter::chatCompletion(const std::string& system_prompt, const std::string& user_text, F32 temperature, std::string& content, std::string& error)
{
    return chatCompletionMessages({ { "system", system_prompt }, { "user", user_text } }, temperature, 600, content, error);
}

// static
bool FSAIWriter::chatCompletionMessages(const std::vector<ChatMessage>& chat_messages, F32 temperature, S32 max_tokens, std::string& content, std::string& error)
{
    const std::string url = trim(gSavedSettings.getString("FSAIWriterEndpoint"));
    if (url.empty())
    {
        error = "No AI server configured (FSAIWriterEndpoint).";
        return false;
    }

    boost::json::object body;
    const std::string model = trim(gSavedSettings.getString("FSAIWriterModel"));
    if (!model.empty())
    {
        body["model"] = model;
    }
    boost::json::array messages;
    for (const ChatMessage& msg : chat_messages)
    {
        messages.push_back(boost::json::object{ { "role", msg.mRole }, { "content", msg.mContent } });
    }
    body["messages"] = std::move(messages);
    body["temperature"] = temperature;
    body["max_tokens"] = max_tokens;
    body["stream"] = false;
    if (gSavedSettings.getBOOL("FSAIWriterDisableThinking"))
    {
        // Reasoning models (Gemma 4, Qwen3, DeepSeek-R1...) otherwise spend
        // the whole token budget "thinking" and return an empty answer.
        // Understood by llama.cpp's llama-server and vLLM; ignored by most
        // other local servers.
        body["chat_template_kwargs"] = boost::json::object{ { "enable_thinking", false } };
    }

    const std::string payload = boost::json::serialize(body);
    LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
    raw->append(payload.data(), payload.size());

    LLCoreHttpUtil::HttpCoroutineAdapter adapter("FSAIWriter", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLSD result = adapter.postRawAndSuspend(request, url, raw, makeOptions(), makeHeaders());

    const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    const LLSD::Binary& raw_body = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
    const std::string response(raw_body.begin(), raw_body.end());

    boost::system::error_code ec;
    boost::json::value root;
    if (!response.empty())
    {
        root = boost::json::parse(response, ec);
    }

    bool success = false;
    if (!status)
    {
        std::string server_error;
        if (!ec.failed() && !response.empty())
        {
            extractContent(root, content, server_error);
        }
        error = server_error.empty() ? describeFailure(result) : server_error;
    }
    else if (response.empty() || ec.failed())
    {
        error = "The AI server returned an invalid response.";
    }
    else if (extractContent(root, content, error))
    {
        content = stripReasoning(content);
        success = !content.empty();
        if (!success)
        {
            error = "The AI returned an empty answer.";
        }
    }

    if (!success)
    {
        LL_WARNS("AIWriter") << "Request to " << url << " failed: " << error << LL_ENDL;
    }
    return success;
}

// static
void FSAIWriter::rewriteCoro(std::string text, std::string style, std::string language, bool more_creative, rewrite_callback_t callback)
{
    Result res;
    const std::string target_language = (language == "en") ? "English" : "Brazilian Portuguese";
    const std::string system_prompt =
        "You are a writing assistant for chat messages in the virtual world Second Life. "
        "The user gives you a message they are about to send. " + getStyleInstruction(style) + " "
        "Write every version in natural, fluent " + target_language + ", the way a native speaker would "
        "write it in a chat; if the message is in another language, translate it. "
        "Keep the original meaning, keep it short like a chat message, and keep names, emojis and "
        "Second Life terms unchanged. "
        "Reply with exactly three alternative versions, one per line, with no numbering, no quotes "
        "and no explanations.";

    std::string content;
    if (chatCompletion(system_prompt, text, more_creative ? 1.0f : 0.6f, content, res.mError))
    {
        res.mSuggestions = parseSuggestions(content);
        res.mSuccess = !res.mSuggestions.empty();
        if (!res.mSuccess)
        {
            res.mError = "The AI returned an empty answer.";
        }
    }
    callback(res);
}

// static
void FSAIWriter::translate(const std::string& text, bool automatic, translate_callback_t callback)
{
    LLCoros::instance().launch("FSAIWriter::translateCoro", [text, automatic, callback]() { translateCoro(text, automatic, callback); });
}

// static
void FSAIWriter::translateCoro(std::string text, bool automatic, translate_callback_t callback)
{
    std::string target = trim(gSavedSettings.getString("FSAIWriterTranslateTo"));
    if (target.empty())
    {
        target = "Brazilian Portuguese";
    }
    const std::string system_prompt =
        "You translate chat messages from the virtual world Second Life. Translate the user's message into natural, "
        "fluent " + target + ", the way a native speaker would write it in a chat. Keep names, emojis, slang meaning "
        "and Second Life terms. " +
        (automatic
            ? "If the message is already in " + target + ", or has nothing to translate (only names, numbers, emojis "
              "or links), reply with exactly " + SAME_MARKER + " and nothing else. "
            : "If the message is already in " + target + ", return it unchanged. ") +
        "Reply with the translation only: no quotes, no notes, no explanations.";

    std::string content;
    std::string error;
    bool success = chatCompletion(system_prompt, text, 0.2f, content, error);
    if (success && automatic)
    {
        std::string normalized = content;
        LLStringUtil::trim(normalized);
        std::string original = text;
        LLStringUtil::trim(original);
        if (normalized.find(SAME_MARKER) != std::string::npos
            || LLStringUtil::compareInsensitive(normalized, original) == 0)
        {
            content.clear(); // already in the target language
        }
    }
    callback(success, content, error);
}

// static
void FSAIWriter::fetchModels(models_callback_t callback)
{
    LLCoros::instance().launch("FSAIWriter::fetchModelsCoro", [callback]() { fetchModelsCoro(callback); });
}

// static
void FSAIWriter::fetchModelsCoro(models_callback_t callback)
{
    std::vector<std::string> models;
    const std::string url = getModelsUrl(gSavedSettings.getString("FSAIWriterEndpoint"));

    LLCoreHttpUtil::HttpCoroutineAdapter adapter("FSAIWriterModels", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLSD result = adapter.getRawAndSuspend(request, url, makeOptions(), makeHeaders());

    const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    if (!status)
    {
        callback(false, models, describeFailure(result));
        return;
    }

    const LLSD::Binary& raw_body = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
    boost::system::error_code ec;
    boost::json::value root = boost::json::parse(std::string(raw_body.begin(), raw_body.end()), ec);
    // OpenAI style: { "data": [ { "id": "model-name" }, ... ] }
    if (!ec.failed() && root.is_object())
    {
        if (const boost::json::value* data = root.as_object().if_contains("data"))
        {
            if (data->is_array())
            {
                for (const boost::json::value& entry : data->as_array())
                {
                    if (entry.is_object() && entry.as_object().if_contains("id") && entry.as_object().at("id").is_string())
                    {
                        models.emplace_back(entry.as_object().at("id").as_string());
                    }
                }
            }
        }
    }
    if (models.empty())
    {
        callback(false, models, "The server did not list any models.");
        return;
    }
    callback(true, models, "");
}

// static
void FSAIWriter::chat(const std::vector<ChatMessage>& messages, chat_callback_t callback)
{
    LLCoros::instance().launch("FSAIWriter::chatCoro", [messages, callback]() { chatCoro(messages, callback); });
}

// static
void FSAIWriter::chatCoro(std::vector<ChatMessage> messages, chat_callback_t callback)
{
    std::string content;
    std::string error;
    const bool success = chatCompletionMessages(messages, 0.7f, 2048, content, error);
    callback(success, content, error);
}

namespace
{
    std::string decodeHtml(const std::string& in)
    {
        // strip tags, then the handful of entities search pages use
        std::string s = boost::regex_replace(in, boost::regex("<[^>]*>"), "");
        static const std::pair<const char*, const char*> entities[] = {
            { "&amp;", "&" }, { "&quot;", "\"" }, { "&#x27;", "'" }, { "&#39;", "'" },
            { "&lt;", "<" }, { "&gt;", ">" }, { "&nbsp;", " " } };
        for (const auto& [from, to] : entities)
        {
            LLStringUtil::replaceString(s, from, to);
        }
        s = boost::regex_replace(s, boost::regex("\\s+"), " ");
        return trim(s);
    }

    std::vector<FSAIWriter::SearchResult> parseDuckDuckGoLite(const std::string& html, size_t max_results)
    {
        std::vector<FSAIWriter::SearchResult> results;
        const boost::regex link_re("<a[^>]*href=\"([^\"]*)\"[^>]*class='result-link'[^>]*>([\\s\\S]*?)</a>");
        const boost::regex snippet_re("<td class='result-snippet'>([\\s\\S]*?)</td>");

        for (boost::sregex_iterator it(html.begin(), html.end(), link_re), end; it != end && results.size() < max_results; ++it)
        {
            FSAIWriter::SearchResult r;
            std::string href = decodeHtml((*it)[1].str());
            // DuckDuckGo wraps the target: //duckduckgo.com/l/?uddg=<escaped url>&rut=...
            const size_t uddg = href.find("uddg=");
            if (uddg != std::string::npos)
            {
                std::string target = href.substr(uddg + 5);
                const size_t amp = target.find('&');
                if (amp != std::string::npos)
                {
                    target = target.substr(0, amp);
                }
                href = LLURI::unescape(target);
            }
            r.mUrl = href;
            r.mTitle = decodeHtml((*it)[2].str());
            results.push_back(r);
        }

        size_t i = 0;
        for (boost::sregex_iterator it(html.begin(), html.end(), snippet_re), end; it != end && i < results.size(); ++it, ++i)
        {
            results[i].mSnippet = decodeHtml((*it)[1].str());
        }
        return results;
    }

    std::vector<FSAIWriter::SearchResult> parseSearxng(const std::string& json, size_t max_results)
    {
        std::vector<FSAIWriter::SearchResult> results;
        boost::system::error_code ec;
        boost::json::value root = boost::json::parse(json, ec);
        if (ec.failed() || !root.is_object())
        {
            return results;
        }
        const boost::json::value* list = root.as_object().if_contains("results");
        if (!list || !list->is_array())
        {
            return results;
        }
        auto field = [](const boost::json::object& o, const char* key) -> std::string
        {
            const boost::json::value* v = o.if_contains(key);
            return (v && v->is_string()) ? std::string(v->as_string()) : std::string();
        };
        for (const boost::json::value& entry : list->as_array())
        {
            if (results.size() >= max_results)
            {
                break;
            }
            if (entry.is_object())
            {
                const boost::json::object& o = entry.as_object();
                results.push_back({ field(o, "title"), field(o, "url"), field(o, "content") });
            }
        }
        return results;
    }
}

// static
void FSAIWriter::webSearch(const std::string& query, search_callback_t callback)
{
    LLCoros::instance().launch("FSAIWriter::webSearchCoro", [query, callback]() { webSearchCoro(query, callback); });
}

// static
void FSAIWriter::webSearchCoro(std::string query, search_callback_t callback)
{
    std::vector<SearchResult> results;
    std::string base = trim(gSavedSettings.getString("FSAIChatbotSearchURL"));
    if (base.empty())
    {
        base = "https://lite.duckduckgo.com/lite/?q=";
    }
    const bool searxng = base.find("format=json") != std::string::npos;
    std::string url;
    if (base.find("{query}") != std::string::npos)
    {
        url = base;
        LLStringUtil::replaceString(url, "{query}", LLURI::escape(query));
    }
    else
    {
        url = base + LLURI::escape(query);
    }

    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    // Plain browser user agent: search pages refuse unknown clients.
    headers->append(HTTP_OUT_HEADER_USER_AGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36");
    LLCore::HttpOptions::ptr_t options = std::make_shared<LLCore::HttpOptions>();
    options->setTimeout(20);
    options->setTransferTimeout(20);
    options->setFollowRedirects(true);
    options->setRetries(0);

    LLCoreHttpUtil::HttpCoroutineAdapter adapter("FSAIChatbotSearch", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLSD result = adapter.getRawAndSuspend(request, url, options, headers);

    const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    if (!status)
    {
        callback(false, results, "Web search failed: " + status.toString());
        return;
    }
    const LLSD::Binary& raw_body = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
    const std::string body(raw_body.begin(), raw_body.end());
    constexpr size_t MAX_RESULTS = 5;
    results = searxng ? parseSearxng(body, MAX_RESULTS) : parseDuckDuckGoLite(body, MAX_RESULTS);
    if (results.empty())
    {
        callback(false, results, "The web search returned no results.");
        return;
    }
    callback(true, results, "");
}
