# Firestorm "Perf + AI": an experimental, vibe-coded fork

> **This is not the official Firestorm.** It is a personal, experimental fork
> of the [Firestorm Viewer](https://github.com/FirestormViewer/phoenix-firestorm),
> the open source viewer for Second Life. It has no affiliation with the
> Firestorm team or with Linden Lab. Use it at your own risk.

## Why this fork exists

The Second Life viewer codebase is **old**. Much of each frame runs on a single
thread, and a lot of heavy work happens on the main thread in the middle of a
frame. On modern many-core CPUs this leaves most of the processor idle while
one core carries everything, and the result is stutter, especially in crowded
places.

I built this fork by **vibe coding**. The changes were written in a
conversation with an AI coding assistant (Claude, by Anthropic), which read the
code, proposed changes, implemented, compiled and fixed them, while I tested
in Second Life and reported what worked and what did not. The goals were:

1. **Performance and frame-time stability:** fewer hitches and higher FPS,
   without lowering global graphics quality.
2. **New features built on a local AI:** a large language model (LLM) running
   on my own machine (llama.cpp, LM Studio, Ollama...). Nothing is sent to
   cloud AI services.

In my own testing it runs better than the original, but this is **not a
formal benchmark**. The viewer includes an on-screen overlay and a
frame-pacing log (see below), so anyone can measure before and after on their
own machine.

Every code change is marked with `<FS:Perf>`. All of the work is on the
`perf-overhaul` branch, starting right after upstream commit `48d525fca3`.

**Downloads:** ready-to-use Windows builds are on the
[Releases page](https://github.com/bastontheking/phoenix-firestorm/releases).
There is an installer and a portable ZIP, each in an **AVX2** version
(recommended for CPUs from 2013 onwards) and a generic version.

---

## Part 1: Performance

The technical details (architecture, bottlenecks found, how to measure) are in
[`doc/performance_overhaul.md`](doc/performance_overhaul.md). Summary:

### 1.1 Job system (parallel work inside a frame)
- New `LL::JobSystem` (`indra/llcommon/lljobsystem.*`): a fork-join
  `parallelFor` that spreads work the frame needs *right now* across the CPU
  cores.
- No locks on the hot path, the calling thread does work too, and the worker
  count is derived automatically from the number of cores.
- An isolated stress test passes every case: full index coverage, nested
  calls, and several threads submitting work at once. On 16 hardware threads
  it ran 10.5× faster than the serial version.
- Setting: `FSJobSystemThreads` (0 = automatic).

### 1.2 Work moved off the main thread (serial → parallel)
| What | Before | Now | Setting |
|---|---|---|---|
| Texture priorities (on-screen area of every face) | serial, main thread | parallel, job system | `FSParallelTextureStats` |
| Low-memory emergency texture re-prioritisation | every texture in a single frame | parallel | — |
| Texture alpha analysis and click (pick) mask | per-pixel pass on the main thread | split across cores, identical results | — |
| Avatar skinning matrix palettes | built one by one inside the draw loops | all visible avatars in parallel, before culling | `FSParallelSkinningPalettes` |

### 1.3 Hitches removed
- **Geometry rebuilds:** Firestorm was given a time budget for these and
  **ignored it**, draining the whole queue in one frame. The budget is now
  enforced. Nearby objects, avatars, attachments, HUDs and meshes that just
  finished loading are still rebuilt immediately; everything else is spread
  over the following frames.
  Settings: `FSBudgetGeometryUpdates`, `FSGeomUpdateMinBudgetMs`,
  `FSGeomUpdateNearDistance`.
- **Loaded meshes:** these used to be deep-copied on the main thread; the data
  is now moved instead. The queue of finished meshes also has a per-frame
  budget (`FSMeshLoadedBudgetMs`).
- **Impostors** (distant avatars drawn as a flat snapshot): every stale one
  used to be regenerated in the same frame. Now only the N stalest are
  refreshed per frame (`FSMaxImpostorUpdatesPerFrame`).
- **Thread pool workers:** these polled with `Sleep(1)` in a loop, which added
  1–2 ms of latency to every decode, fetch and mesh task. They now wake up on
  notification.
- **FPS limiter:** this truncated its wait to whole milliseconds. It now
  schedules frames against deadlines, with sub-millisecond accuracy.

### 1.4 Adaptive quality (frame budget)
- New `LLFrameBudget`: compares frame time against a target
  (`FSFrameBudgetTargetFPS`, 60 by default) and derives a "pressure" value
  from 0 to 1.
- When frames run over budget, it reduces only things you are unlikely to
  notice, in this order:
  - the animation rate of avatars that are small on screen;
  - the LOD of objects that are small on screen;
  - how many impostors are refreshed per frame;
  - time spent on rebuilds that can wait.
- It **never** degrades what is large or close to the camera, attachments or
  HUDs. Quality comes back gradually once there is headroom.
- It ignores frames while the window is unfocused or minimized, and respects
  the FPS limiter.
- Settings: `FSAdaptiveQuality`, `FSAvatarAnimLOD`.

### 1.5 Animation LOD
- Avatars that are small on screen animate at 1/2, 1/3 or 1/4 of the frame
  rate. Their pose is held between updates, but animations keep their real
  speed.

### 1.6 Measuring
- **On-screen overlay:** *Advanced > Show Info > Show Performance Stats*
  (`FSShowPerfStats`). It shows:
  - average FPS, plus median, p99 and p99.9 frame time;
  - 1% low and 0.1% low;
  - spikes (frames slower than twice the median);
  - frame-budget pressure;
  - job system utilisation;
  - deferred rebuilds;
  - avatars with a reduced animation rate.
- **Log:** `FSLogFramePacing` writes a `FramePacing` line to the log every 5 s.
- Almost every optimisation has a switch in *Debug Settings*. With all of them
  off you get the old behaviour **in the same build**, which makes before and
  after comparisons easy.

---

## Part 2: AI features (with your own LLM)

All AI features use a server you run yourself that speaks the **OpenAI
"chat completions" API**:
[llama.cpp](https://github.com/ggml-org/llama.cpp) (`llama-server`),
LM Studio, Ollama, vLLM and so on. Text is sent **only** to the address you
configure. The one external service is the ChatBot's optional web search,
which is off by default; when it is on, only the text of your question goes
to DuckDuckGo.

**Every AI feature is off by default**, so the viewer behaves like a normal
Firestorm until you set up a server. The chat bar only shows a ⚙ button: open
it, enter your server, then switch on the features you want (writing
suggestions, ChatBot tab, automatic translation, the *Translate message*
menu). Turning suggestions off hides the language and style boxes again, and
the ChatBot tab appears or disappears immediately.

For "reasoning" models (Gemma 4, Qwen3...), the viewer asks the server to skip
the thinking phase (`FSAIWriterDisableThinking`). Without that, the model can
spend its entire token budget thinking and return an empty answer.

### 2.1 Settings window (⚙ button)
The gear button in the chat bar or in the ChatBot opens this window:
- **AI features:** switches for writing suggestions and the ChatBot tab
  (translation switches are further down).
- **Server:** the endpoint, e.g. `http://IP:8080/v1/chat/completions`
  (`FSAIWriterEndpoint`).
- **Model:** which model to use. **Load models** fetches the list from
  `/v1/models` (`FSAIWriterModel`).
- **Skip model reasoning:** turns the model's thinking phase off or on.
- **Translation:** automatic translation, the right-click menu item, and the
  target language.
- **ChatBot:** internet access, showing sources, Second Life context, and
  **Clear history**.
- **Test connection:** runs a short test translation against the server.

### 2.2 Writing suggestions in the chat bar (nearby chat and IMs)
```
[ suggestions: appear on their own while you type           ]
[input] [PT-BR/EN/ES/FR/DE/IT/JA] [Style ▾] [⚙] [emoji] [send]
```
- About **1 second** after you stop typing (`FSAIWriterAutoSuggestDelay`),
  **3 suggestions** for your sentence appear above the input.
- **Language:** the language the suggestions are written in. If your text is
  in another language, it is translated.
- **Style:** Nicer, Formal, Casual, Romantic, Funny, or Fix only (just
  corrects mistakes).
- Clicking a suggestion puts it in the input. **Nothing is ever sent
  automatically**; you still press Enter.
- The suggestions strip hides when the input is empty and after sending.
- `/commands` and hidden chat windows are ignored.
- Settings: `FSAIWriterAutoSuggest`, `FSAIWriterLanguage`, `FSAIWriterStyle`,
  `FSAIWriterShowButton`.

### 2.3 Message translation
- **Right-click** any message in nearby chat or an IM and pick
  *Translate message (AI)*. The translation appears **right below** the
  original, in italics, in its own color and the regular chat font size:
  ```
  [20:14] Someone: Oi, tudo bem?
  [Translator] Someone: Hi, how are you?
  ```
- **Automatic** (`FSAIWriterAutoTranslate`): every incoming message in another
  language gets its translation underneath. These are skipped:
  - messages already in the target language;
  - your own messages;
  - system notices;
  - object chat;
  - history loaded from the log.
- Each line is translated **only once**, and a translation is never
  translated again. Translation lines exist **only on your screen**; nothing
  is sent to Second Life.
- Target languages: Brazilian Portuguese (default), English, Spanish, French,
  German, Italian, Japanese, Korean, Simplified Chinese, Russian, Turkish,
  Dutch, Polish, or type any other language name.
- Settings: `FSAIWriterTranslateTo`, `FSAIWriterTranslatorTag` ("Translator"),
  `FSAIWriterTranslatorColor`, `FSAIWriterTranslateMenu`.

### 2.4 ChatBot (tab in the Conversations window)
A locked tab right below **Nearby Chat**, with a robot icon:
```
[ conversation ................................................ ]
[ message the ChatBot ........ ] [⚙] [Clear] [Send ▴]
```
- A private conversation with your LLM, **with history**. The history is saved
  per account (`ai_chatbot_history.xml`), and the last
  `FSAIChatbotHistoryLength` messages are sent as context.
- **Send ▴:** the arrow switches between **Send** and **Send w/o history**.
  The second mode is a one-off question that neither reads nor changes the
  conversation; it is shown in italics.
- **Clear** (or typing `/clear`) erases the conversation.
- **Internet access** (optional, `FSAIChatbotWebSearch`): searches the web for
  your question (DuckDuckGo Lite by default, or your own SearXNG instance via
  `FSAIChatbotSearchURL`) and gives the results to the AI. Sources can be
  listed below the answer as clickable links (`FSAIChatbotShowSources`).
- **Knows Second Life** (`FSAIChatbotSLContext`):
  - Every question carries your avatar, the region, avatars within 256 m
    (with distances) and online friends.
  - When a question mentions a **nearby avatar or a friend** (by display
    name, username or a distinctive first name), the viewer fetches their
    **profile**: bio, account age, partner, groups, picks and web profile.
  - Example: *"tell me about [avatar name]"*.
  - Profile text is treated only as that person's self-description, never as
    instructions to the AI.
  - RLV restrictions (@shownames, @showloc) are respected.
- To hide the tab: `FSAIChatbotEnabled`.

---

## Part 3: Chat history controls

Both features below are **local only**: they change what is stored on your
computer. Nothing is sent to Second Life, and the other people in the
conversation keep their own copies.

### 3.1 Delete chat history (one conversation)
- **Delete chat history...** removes the saved transcript of **one** person
  or group, including the automatic backup copies, and clears the
  conversation window if it is open. It asks for confirmation first.
- Where to find it:
  - the trash button in the IM window toolbar, next to the history button;
  - right-click on a person in **Contacts**, the **radar** and name lists;
  - right-click on a conversation in the **Conversation Log**
    (*Comm > Conversation Log*).
- Firestorm's existing *Preferences > Privacy > Delete transcripts* still
  deletes **every** conversation at once.

### 3.2 Delete for me (one message)
- **Right-click** any message in nearby chat or an IM and pick
  **Delete for me**.
- The **whole message** is removed, including every line of a long or
  multi-line message and its AI translation line if there is one. It is
  removed from:
  - the window;
  - the saved transcript;
  - the window's in-memory list, so it does not come back when the window
    reloads.
- Each window remembers the exact text of its last 500 messages. For older
  text, only the clicked line is removed, and only from the screen.
- In the saved file, the message is matched by sender and text, newest
  first. If the same person sent the exact same text more than once, the
  newest copy is the one removed from the file.

---

## Part 4: Smaller changes
- **Spell checker:** this already existed in Firestorm (Hunspell, with
  pt-BR included). The only change was making the dictionaries available in
  development builds.
- **Fast Timers:** removed the Ctrl+Shift+9 shortcut. On Brazilian ABNT2
  keyboards, Shift+9 is `(`, so the profiler kept opening by accident. It is
  still available under *Advanced > Consoles*.
- **Building from paths with spaces:** fixed the resource compiler (`rc.exe`)
  include quoting in CMake, and quoted the paths in the generated NSIS
  installer script.
- **Installer CPU check:** the "AVX2 build available" and "your CPU lacks
  AVX2" prompts link to this fork's releases page. Before, they showed
  `<NO-URL>` for unofficial builds.
- **Kept in sync with upstream:** official Firestorm `master` is merged in
  regularly, so its fixes are included.

---

## Building (Windows)

The steps are the same as in [`doc/building_windows.md`](doc/building_windows.md).
I used Visual Studio 2026 (toolset 14.4x+) with `AUTOBUILD_VSVER=180`:

```bat
set AUTOBUILD_VSVER=180
set AUTOBUILD_VARIABLES_FILE=<path>\fs-build-variables\variables
autobuild configure -A 64 -c ReleaseFS_open -- --chan Perf -DLL_TESTS:BOOL=FALSE
autobuild build -A 64 -c ReleaseFS_open --no-configure
```

Without `--package` you get a development build, which must be started with
the **working directory set to `indra\newview`**, for example:
```bat
cd /d phoenix-firestorm\indra\newview
start "" ..\..\build-vc180-64\newview\Release\firestorm-bin.exe
```

---

## Important notes
- **This is not the official Firestorm.** Please do not ask the Firestorm team
  for support with this fork.
- If you **distribute binaries** to other people:
  - use a different name and none of the Firestorm logos;
  - follow Linden Lab's trademark policy and the
    [Third Party Viewer Policy](https://secondlife.com/corporate/third-party-viewers);
  - remember that the AI features can send other people's text (incoming
    messages, profiles) to the configured server. Make that clear to your
    users, and keep those features off by default.
- This is **vibe-coded** software. It was reviewed and tested, but it may
  still have bugs.

## License
The code remains under the **GNU LGPL 2.1**, like the original project (see
[`doc/LICENSE-source.txt`](doc/LICENSE-source.txt)). The new files added by
this fork are LGPL 2.1 as well. Artwork and trademarks remain under the
original licenses and policies of Linden Lab and The Phoenix Firestorm
Project.
