> [!WARNING]
> **Unofficial, experimental build.** This is a personal fork of the Firestorm Viewer, built through AI-assisted development. It is **not** an official Firestorm release and is not affiliated with The Phoenix Firestorm Project or Linden Lab. Please do not ask the Firestorm team for support with it. Use at your own risk.

Builds of the `perf-overhaul` branch for **Windows, Linux and macOS**, based on **Firestorm 7.3.0** and up to date with the official Firestorm `master`. They use the open source configuration: no FMOD (sound uses OpenAL) and no Kakadu. OpenSim grid support is enabled, hence "FirestormOS" in the file names. All files are built by GitHub Actions from this repository.

## What's new in this version
- **Now on Firestorm 7.3.0**, with all official fixes up to October 28, 2026, including Lua (SLua) script support and the new SDL3 Linux backend.
- **First Linux and macOS builds** of this fork.
- **Mesh upload picks up every LOD file:** pick the high LOD file and the medium, low, lowest and physics files next to it are loaded too, whether they are named `_LOD2`/`_PHYS`, `_LOD_medium`/`_physics`, `_medium`/`_low`, or the Portuguese `_LOD_medio`/`_fisica`.
- **Ctrl+Shift+U** opens *Build > Upload > Mesh Model*.
- **Advanced menu** (Ctrl+Alt+D): *Fly Override on Startup* and *Clear Cache on Exit*.
- **Linux:** open and save dialogs remember the last folder you used.
- **Building with Claude:** *Help > MCP Server (Claude)* starts a local bridge that lets Claude build in-world through the viewer, including LSL and Lua scripts. It needs the separate bridge scripts; see Part 6 of the fork notes.

## Everything that is different from Firestorm
- **Performance:** a job system spreads per-frame work across CPU cores; geometry rebuilds, loaded meshes and impostor refreshes respect a per-frame budget; adaptive quality lowers only what you won't notice; a precise FPS limiter; and a frame-pacing overlay (*Advanced > Show Info > Show Performance Stats*).
- **AI features** (off by default, using *your own* LLM server such as llama.cpp, LM Studio or Ollama): writing suggestions in the chat bar, message translation, and a ChatBot tab that knows your Second Life surroundings. Nothing is sent to cloud AI services.
- **Chat history controls** (local only): delete one conversation's saved history, or delete a single message for yourself.
- **Look-at highlight:** large markers showing where other avatars are looking and what they are doing.

Full details, settings and caveats: **[FORK-PERF-AI.md](https://github.com/bastontheking/phoenix-firestorm/blob/perf-overhaul/FORK-PERF-AI.md)**

## Downloads
Pick the file for your system. **Almost every CPU from 2013 onwards supports AVX2** (Intel Haswell / 4th gen Core or newer, AMD Ryzen and most of the FX line), and the AVX2 build is the faster one.

| System | File |
|---|---|
| **Windows**, AVX2 (recommended) | `…Perf_AVX2…_Setup.exe` (installer) or `…Perf_AVX2…_Portable.zip` |
| Windows, older CPUs without AVX2 | `…Perf…_Setup.exe` / `…_Portable.zip` without `AVX2` in the name |
| **Linux**, AVX2 (recommended) | `…Perf_AVX2…tar.xz` |
| Linux, older CPUs without AVX2 | `…tar.xz` without `AVX2` in the name |
| **macOS** | `….dmg` |

- **Windows:** the installers are not code-signed, so SmartScreen may warn you. Choose *More info > Run anyway*. Portable: unzip anywhere and run the `.exe` inside.
- **Linux:** extract the archive anywhere and run `./firestorm` inside the folder. Optionally run `./install.sh` to add it to your applications menu.
- **macOS:** the app is not notarized. On first launch, right-click it and choose *Open*, or run `xattr -dr com.apple.quarantine /path/to/Firestorm*.app`.

This build uses its own channel (`Firestorm-Perf`), but its settings and logs share the normal Firestorm user folder (`%APPDATA%\Firestorm_x64` on Windows, `~/.firestorm_x64` on Linux, `~/Library/Application Support/Firestorm_x64` on macOS). Back that folder up if you also use the official viewer.

Source code: LGPL 2.1, this repository.
