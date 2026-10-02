# Performance overhaul (branch `perf-overhaul`)

This branch reduces main-thread work and frame-time spikes without lowering
global quality settings. Everything that changes per-frame behaviour can be
switched off from Debug Settings, so the same build can be measured with and
without each change.

## Architecture

```
idle()                                   display()
  avatars: updateCharacter                 updateMatrixPalettesParallel  ──► JobSystem (1 job / avatar)
    └─ animation LOD (LLFrameBudget)       updateGeom  (time-budgeted, near/avatar/HUD first)
  meshes loaded  (time-budgeted, move)     updateCull
                                           generateSunShadow
                                           updateImpostors (N stalest / frame)
                                           updateImages
                                             └─ computeVirtualSizesParallel ──► JobSystem
                                                  (face pixel area + per-texture vsize)
                                             └─ createTexture
                                                  └─ analyzeAlpha / pick mask ──► JobSystem
frame end: LLViewerStats::updateFrameStats ─► LLFrameBudget::update (pressure)
```

### `LL::JobSystem` (`indra/llcommon/lljobsystem.*`)

Fork-join `parallelFor` for work the current frame needs immediately. It
complements `LL::WorkQueue`/`LL::ThreadPool`, which remain the tool for
asynchronous work (decode, fetch, disk).

- Workers: `hardware_concurrency - 1 - reserved`, where reserved counts the
  fetch/cache threads and half of the image decode threads
  (`FSJobSystemThreads` overrides, 0 = auto).
- The calling thread executes chunks too; chunks are claimed through a single
  64-bit atomic word (generation | chunk count | next chunk), so there is no
  lock on the hot path and a late worker can never run a chunk of a batch it
  did not observe.
- One batch in flight at a time; a nested call or a call from a second thread
  runs serially instead of waiting, so it can not deadlock.
- Contract for job bodies: write only data owned by the index range, read
  shared data that is immutable for the duration of the call, never call GL /
  UI / main-thread-only APIs. Every use site documents its ownership model.

Stand-alone stress test (coverage of every index, nesting, 4 concurrent
submitters): all pass; 10.5x speedup on a compute-bound loop with 13 workers
on a 16-thread CPU; ~3 µs dispatch overhead per small batch.

### `LLFrameBudget` (`indra/newview/llframebudget.*`)

Smoothed frame time against `FSFrameBudgetTargetFPS` (default 60) produces a
`pressure` value in [0, 1] that rises quickly when over budget and recovers
slowly (about 10 s) when there is headroom. It drives only perceptually minor
knobs, least visible first:

| Knob | At pressure 0 | Under pressure |
|---|---|---|
| Animation rate of avatars small on screen | 1/2 rate below 10% of screen height, 1/3 below 4%, 1/4 below 1.5% | tiers widen up to 3x, max period 6 |
| LOD factor of objects small on screen (projected tan < 0.24) | unchanged | up to -35%, in 3 discrete steps (no LOD flip-flop) |
| Impostor refreshes per frame | `FSMaxImpostorUpdatesPerFrame` (8) | down to 2 |
| Deferrable geometry rebuild time | max(requested, `FSGeomUpdateMinBudgetMs`) | halved |

Objects that are large on screen, near the camera, attached to avatars, or
HUDs are never degraded. With `FSAdaptiveQuality` off, pressure stays 0.

## Bottlenecks found and what changed

| Bottleneck | Change |
|---|---|
| `LLPipeline::updateGeom` received a time budget but never checked it; the whole rebuild queue was drained every frame (LOD storms, teleports, region crossings) | Budgeted. Drawables that are near, avatars, attachments, rigged or HUD are always rebuilt; others are rebuilt until the budget is spent (minimum 16 per frame) |
| Texture priority: `calcPixelArea` + face scan per texture, serial | Faces claimed serially, pixel areas and per-texture scans on the job system, bookkeeping stays serial and time-sliced |
| Low-memory emergency path re-prioritised every texture serially in one frame | Same parallel scan |
| `analyzeAlpha` / `updatePickMask`: per-pixel passes on the upload thread for every texture | Split across the job system for images of 256x256 or more; results identical |
| Loaded meshes were deep-copied into the system volume on the main thread, and the loaded queue was drained without limit | Faces moved instead of copied; queue applied under `FSMeshLoadedBudgetMs`, the rest re-queued |
| Every stale impostor regenerated in the same frame (each one is a state sort + render pass) | Only the N stalest per frame |
| Skinning palettes built lazily inside the shadow and main draw loops | Rebuilt for all visible avatars in parallel before culling |
| Thread pool workers polled with `Sleep(1)` and ignored `notify()`: 1-2 ms latency on every decode/fetch/mesh task and reply | Condition-variable wait woken by `notify()` |
| FPS limiter truncated to whole milliseconds and restarted its timer after sleeping | Deadline-based pacing, coarse sleep + short yield |
| Distant avatars evaluated all motions every frame | Animation LOD by screen size; pose held, timer keeps running so speed is exact |

Investigated and left unchanged:

- Octree `balance()` every frame: only touches the root, O(1).
- Occlusion queries: already asynchronous (`GL_QUERY_RESULT_AVAILABLE`).
- `glGetError`/`glFinish`: not in the release per-frame path.

## Measuring

1. Enable `FSShowPerfStats` (Advanced > Show Info > Show Performance Stats)
   and/or `FSLogFramePacing`.
2. Stand in a fixed spot in a heavy scene (club, store, mainland with many
   avatars). Wait for everything to rez.
3. Record about 60 s with the defaults, then 60 s with the toggles below off,
   and compare the `FramePacing` lines in the log:

   ```
   grep FramePacing Firestorm.log
   ```

   Each line includes avg FPS, median/p95/p99/p99.9 frame time, 1%/0.1% lows
   and spikes (> 2x median).

| Setting | Default | "Before" value |
|---|---|---|
| `FSParallelTextureStats` | 1 | 0 |
| `FSParallelSkinningPalettes` | 1 | 0 |
| `FSBudgetGeometryUpdates` | 1 | 0 |
| `FSAvatarAnimLOD` | 1 | 0 |
| `FSAdaptiveQuality` | 1 | 0 |
| `FSMaxImpostorUpdatesPerFrame` | 8 | 1000 |
| `FSMeshLoadedBudgetMs` | 2 | 1000 |
| `FSJobSystemThreads` | 0 (auto) | 1 (single worker; restart needed) |

The thread pool wake-up, mesh face move, FPS limiter and alpha analysis
changes are not toggleable.

## Not done yet (recommended next steps)

- Parallel culling: `markNotCulled` mutates group state and pushes into the
  shared cull result. It needs a split into a parallel frustum traversal that
  collects candidates per thread and a serial apply phase.
- Budgeting `rebuildPriorityGroups` and the `postSort` `rebuildGeom` loop.
  `rebuildGeom` uses static face arrays and allocates GL buffers, so it must
  first be split into a CPU fill phase and a GL allocate/flush phase.
- Updating shadow cascades 2-3 and spot shadows every other frame. This is
  a visual trade-off and needs testing.
- `RenderGLMultiThreadedTextures` is off by default on Windows NVIDIA/Intel,
  so GL uploads stay on the main thread. The fence path exists; it needs
  validation per driver.
- Moving alpha analysis fully to the decode threads, with the result carried
  next to the raw image.
- A persistent cache for decoded/derived mesh LOD data.
