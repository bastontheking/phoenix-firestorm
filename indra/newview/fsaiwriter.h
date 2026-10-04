/**
 * @file fsaiwriter.h
 * @brief Chat writing assistant backed by a user-configured, OpenAI-compatible LLM endpoint
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

#ifndef FS_AIWRITER_H
#define FS_AIWRITER_H

#include <functional>
#include <string>
#include <vector>

/**
 * Sends a chat message the user is writing to an LLM server the user runs
 * themselves (LM Studio, Ollama, llama.cpp server, vLLM, ... anything that
 * speaks the OpenAI "chat completions" API) and returns suggested rewrites.
 *
 * Nothing is sent anywhere unless the user explicitly asks for a suggestion,
 * and only to the endpoint configured in FSAIWriterEndpoint.
 *
 * Callbacks run on the main thread (from a coroutine), so they may touch UI.
 */
class FSAIWriter
{
public:
    struct Result
    {
        bool mSuccess = false;
        std::vector<std::string> mSuggestions;
        std::string mError;
    };
    using rewrite_callback_t = std::function<void(const Result&)>;
    using models_callback_t = std::function<void(bool success, const std::vector<std::string>& models, const std::string& error)>;
    using translate_callback_t = std::function<void(bool success, const std::string& translation, const std::string& error)>;

    // Chat bot: role ("system" / "user" / "assistant") and content.
    struct ChatMessage
    {
        std::string mRole;
        std::string mContent;
    };
    using chat_callback_t = std::function<void(bool success, const std::string& reply, const std::string& error)>;

    struct SearchResult
    {
        std::string mTitle;
        std::string mUrl;
        std::string mSnippet;
    };
    using search_callback_t = std::function<void(bool success, const std::vector<SearchResult>& results, const std::string& error)>;

    // Style keys understood by rewrite(); see getStyleInstruction().
    static const std::vector<std::string>& getStyleKeys();

    // Ask for up to three rewrites of text in the given style, written in
    // language ("pt" = Brazilian Portuguese, "en" = English; translating if
    // needed). When more_creative is set, a higher temperature is used.
    static void rewrite(const std::string& text, const std::string& style, const std::string& language, bool more_creative, rewrite_callback_t callback);

    // Translate a received chat message into FSAIWriterTranslateTo.
    // In automatic mode a message that is already in the target language
    // yields success with an empty translation (nothing to show).
    static void translate(const std::string& text, bool automatic, translate_callback_t callback);

    // Free-form conversation with the given messages (system prompt first).
    static void chat(const std::vector<ChatMessage>& messages, chat_callback_t callback);

    // Web search (DuckDuckGo Lite by default, or a SearXNG instance whose
    // URL contains "format=json"), see FSAIChatbotSearchURL.
    static void webSearch(const std::string& query, search_callback_t callback);

    // List the models offered by the server (GET <base>/models).
    static void fetchModels(models_callback_t callback);

    // Exposed for testing/diagnostics.
    static std::vector<std::string> parseSuggestions(const std::string& content);
    static std::string getModelsUrl(const std::string& chat_url);

private:
    static std::string getStyleInstruction(const std::string& style);
    static void rewriteCoro(std::string text, std::string style, std::string language, bool more_creative, rewrite_callback_t callback);
    static void translateCoro(std::string text, bool automatic, translate_callback_t callback);
    // POST a chat completion; returns true and the assistant text on success.
    static bool chatCompletion(const std::string& system_prompt, const std::string& user_text, F32 temperature, std::string& content, std::string& error);
    static bool chatCompletionMessages(const std::vector<ChatMessage>& messages, F32 temperature, S32 max_tokens, std::string& content, std::string& error);
    static void chatCoro(std::vector<ChatMessage> messages, chat_callback_t callback);
    static void webSearchCoro(std::string query, search_callback_t callback);
    static std::string stripReasoning(const std::string& content);
    static void fetchModelsCoro(models_callback_t callback);
};

#endif // FS_AIWRITER_H
