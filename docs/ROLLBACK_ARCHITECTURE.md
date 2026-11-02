# Rollback: a map of the code

This is a guide for reading the rollback code. It describes what the files do and how a frame moves through them; it
does not change any code. It was written from reading the sources of upstream `fea28c1` (the file headers, the
function lists and the flow of a pass). Sections say when something was read only in outline, and the last section
lists what was not checked. If this document and the code disagree, the code is right: please fix the document.

Sizes of the files at that commit: `r1_state.c` 2074 lines, `r1_ring.c` 1058, `rb_net.c` 909, `rb_learned.c` 441,
`rb_rollback.c` 424, `rb_wire.c` 67, `rb_input.c` 79, `rb_timesync.c` 39, `rb.h` 64.

## 1. The idea in one page

Rollback needs two things that this project builds in two separate layers:

1. **Netcode** (what to run): both games run the match as a *local* versus match from the same start; only the
   pads of ports 1 and 2 are exchanged. A game predicts the other player's pad, and when the real pad arrives and
   differs ("wrong guess at frame m") it must go back to the state before frame m and run the frames again with the
   right pads. Files: `rb.h`, `rb_input.c`, `rb_wire.c`, `rb_timesync.c`, `rb_net.c`, transport `net_relay.c`.
2. **State save / restore** (how to go back): the game is a recompiled binary, so "the state" is not a small
   struct but the memory of the process: about 62 GB of address space of the game's heap (a few GB committed), the
   image and modules, stacks, thread-local blocks, plus thread positions and fibers. Files: `r1_state.c`,
   `r1_ring.c`, `rb_learned.c`.

A glue file, `rb_rollback.c`, sits between them: at a safe point after every simulation pass it applies the received
messages, decides whether to roll back, and drives the ring.

A normal rollback library (GGPO, GekkoNet, ...) replaces layer 1 only; it expects the game to provide
`save_state` / `load_state`, which is layer 2 here and is where most of the code is.

## 2. Vocabulary

| Term | Meaning (as the code uses it) |
|---|---|
| pass | one run of the game's main loop iteration; a *real* pass advances the fight, a *re-run* pass repeats a frame after a rollback (`g_ra_phase == 2`: pads from the input queue, nothing presented) |
| frame | a fight frame; the ring's base capture is frame 0, real pass n is frame n |
| safe point (H2) | the place after a pass where the game's threads are quiet enough to capture or load: the kick after the submission at guest `0x7103749b24` |
| window 1 | the game's big address reservation (`nn::os::AllocateAddressRegion`, 62 GB reserved, made with `MEM_WRITE_WATCH` in `nnsdk.c`) |
| window 2 | the guest image and the loaded modules (`.data` / `.bss`), guest addresses `0x7100000000..0x7300000000` |
| extras | committed ranges outside both windows that r1 copies whole: main's guest stack, TLS blocks, fighter DLL data, the catch table, the malloc arena, and any other big reservation |
| base | the first full save of a fight; the *flat shadow* keeps a copy of every page at that moment |
| ring | `RG_MAXSLOT` (16) slots; default depth 7 frames back (8 states), `SSBU_RB_DEPTH` |
| slot | one captured frame: main's registers, the other threads' wait places and registers, fibers' host state, command-buffer recordings, and an undo log of pages |
| undo log | per slot, a list of (page id, previous buffer): the values at the previous capture, moved, not copied |
| page id | global index of a tracked page: window 1 pages, then window 2 pages, then extras pages |
| learned set | the pages written since the base (see section 5); only these are compared / copied |
| hold / park | stopping the other guest threads at a waiting call while memory is copied (`park`, `ra_hold_*`) |
| presentation pool | a separate reservation for three allocation sites known not to be simulation state; never saved |

## 3. Who calls whom

```
nnhid.c fill_pad ---> r1_pad / r1_pad_id ---> rbg_pad (rb_rollback.c)   the pad the game reads for port 1 / 2
                                                    |  rbn_local_pad / rbn_pad (rb_net.c -> rb_input.c)
net_relay.c <--> rb_net.c  (receive queue, RTT / clock offset, checksums, session report)
                    uses rb_input.c  input queues, prediction, mismatch
                         rb_wire.c   message encode / decode
                         rb_timesync.c  how far ahead of the peer this side runs

main loop of the game
   r1_frame_end / r1_poll (r1_state.c hooks)
      rgs_step (r1_ring.c) at the safe point of each pass
         rbg_step_body (rb_rollback.c)
            rbn_drain: apply received messages        -> a wrong guess at frame m?
            r1_ring_load(m - 1)  (r1_ring.c)           -> restores memory, threads, fibers
            g_ra_phase = 2: the next passes re-run with the corrected pads
            r1_ring_capture(frame) after each pass     -> new slot
            wait for the 60 Hz tick / for the peer     -> pacing
```

`r1_state.c` includes `r1_ring.c`, which includes `rb_learned.c`; `r1_state.c` also includes `rb_rollback.c`, and
`rb_net.c` includes `rb_input.c`, `rb_wire.c`, `rb_timesync.c`. These are textual includes of `.c` files, so every
`static` in them is shared and the build tool does not see them as dependencies (`BUILDING.md`; touch
`r1_state.c` after editing one of them).

## 4. The files

### 4.1 Netcode (game-free; `rb.h` is its whole public API)

- `rb.h`: `RbPad`, the input-queue functions, the wire format, the time-sync decision. Plain C, no Windows, no game.
- `rb_input.c`: per player, the pads received and the pads *predicted* for frames not received yet; the local pad is
  kept for `frame + delay`. A remote pad that differs from the prediction records a mismatch; its frame is where a
  rollback starts. Sticks inside `RB_STICK_REST` count as 0 so noise does not roll back.
- `rb_wire.c`: PADS / ACK / PING / PONG messages, little-endian, lengths checked.
- `rb_timesync.c`: every 30 frames, the mean of the middle third of the last 30 offset samples decides to run 0.5 %
  slower or 1 % faster (thresholds from Slippi's study, no code copied).
- `tests/rb_test.c`: the only automated test of this layer (a toy simulation behind a lossy link must end equal
  to a run with all pads known). It is not listed in any build script that I found; see the last section.
- `rb_net.c` (not game-free: Windows, uses `net_relay.c`): the session. Modes by `SSBU_RB_NET`: `net` (two games) or
  `loop` (one game and a bot whose pads arrive late). Also: handshake, delay choice from the measured round trip,
  per-frame checksums (`SSBU_RB_NET_LOG`), a test bot, the end-of-match report.

### 4.2 The glue: `rb_rollback.c` (424 lines, "the game side")

Entry points called from the game's hooks: `rbg_step` (each safe point), `rbg_pad` (pads), `rbg_fight_off` (fight
over), `rbg_report`. `rbg_rollback` picks the newest ring slot at or before `m - 1`, calls `r1_ring_load`, sets the
re-run (`g_rgs_left`, `g_rgs_next`, `g_ra_phase = 2`) and counts. If the load is refused (a thread still running)
the wrong guess stays pending for the next tick. If the ring does not exist (not enough memory) the match runs in
lockstep: every pass waits for the other player's pad. A room fight whose match was not agreed (`rbn_match_cancelled()
== 2`) is left to the game's own netcode: no ring, pads untouched. It also contains test hooks (`SSBU_RB_NET_TEST_*`).

### 4.3 The ring: `r1_ring.c` (1058 lines)

Design ("pointer shadow + undo log"), from its header:

- `g_rg_idx[id]` says where the page's value *at the newest capture* is: 0 = the flat shadow of the base, 1 = decommitted
  by the game, >= 2 = a 4 KB buffer of the page pool.
- `r1_ring_capture(frame, c)`: holds the other threads (`ra_hold_quiet`), then every page written since the previous
  capture (and every extras page that differs) gets a new pool buffer with its current bytes; the slot keeps the undo
  log; plus main's registers, the other threads' places, fibers, command-buffer recordings.
- `r1_ring_load(frame, c)`: applies the undo logs of the slots newer than `frame`, newest first, pointers only; then
  every page they name, every page written since the newest capture and every extras page is copied once from its
  value at `frame` into memory; threads, fibers and command buffers are restored; slots newer than `frame` are
  dropped. Returns 1 loaded, 0 not in the ring, -1 refused for now.
- `r1_ring_rebase`: a new base inside a fight. `r1_ring_release`: every slot dropped.
- The second half of the file (from `rgs_*` on) is the **rollback simulation** (`SSBU_RB_SIM`) and its profilers and
  verifier (`SSBU_RB_SIM_VERIFY`, `rgv_*`), used by `tests/rb_sim_cmp.py`. It is test and measurement code living
  in the same file as the production capture / load.
- The online rollback shares `rgs_*` with the simulation: `rgs_wanted` turns the driver on for `SSBU_RB_NET` too.

### 4.4 State infrastructure: `r1_state.c` (2074 lines)

It is organised by *rounds*, in the order they were developed, and the rounds are kept side by side:

| Section (comment banner) | Used by rollback? |
|---|---|
| ranges (`add_range`, `find_heaps`, `collect`) | yes: which memory is tracked (windows, extras) |
| nn::os objects, `park` / `unpark`, thread reports | yes: holding the other guest threads at a wait |
| the job system's fibers (`fibers_save`, `fibers_restore_all`, ...) | yes |
| save / restore, `verify` (R1, `SSBU_R1`) | test only: the first, full save |
| R1 round 2: the fast save (`SSBU_R1_FAST`) | test only |
| R1 round 3: the exact written set (`SSBU_R1_MARK`, `mark_*`, `cp_*`) | `mark_setup`, the parallel page copy (`cp_run`) and the flat shadow are used by the ring; the marks need a marked game DLL, which the normal build does not have |
| R1 round 4: who writes without marking (`SSBU_R1_MARK_PROT`) | debug |
| R1 round 5: run-ahead of one frame (`ra_*`, `SSBU_RUNAHEAD`) | `ra_hold_*`, `ra_kick*`, `ra_frame_end` (the safe point and the task-worker hold) are shared with the ring |
| hooks (`r1_frame_end`, `r1_poll`, `r1_pad`, `r1_init`) | yes: the entry points |
| the guest C heap inside the snapshot (`ar_alloc`, `SSBU_R1` only) | test only |

So most of this file is shared plumbing, and some of it is earlier experiments that the rollback does not need. Which
parts are which is the first thing a refactor has to decide; this table is an outline, not a verified list.

### 4.5 The learned set: `rb_learned.c` (441 lines, "R4")

Replaces the marks on the normal game build. Its header explains it in full; in short:

1. At the base every committed read-write page of windows 1 and 2 that is simulation (not a host stack, not audio output)
   is made read-only.
2. The first write faults into `ls_veh`; the page becomes writable for good and joins the *learned set* (a list of ids).
   Until then its bytes are those of the base, which the shadow holds, so every older state finds the right value.
3. A capture takes every learned page (compared with its newest value first when `SSBU_LS_CMP`, default on, so only
   changed pages are copied); a load restores every learned page.
4. Host code that writes guest memory through the kernel cannot fault: it calls `r1_wrote` first (`ls_range`). A
   decommit makes the pages learned; a commit during a fight makes new pages read-only.
5. At the end of a fight everything is writable again; the next base starts with an empty set.
6. `SSBU_LS_DECAY=<n>` (default 16 since rb6, `=0` off): pages unchanged for `n` captures go read-only again.

It also holds the **presentation pool** (hooks on the game's jemalloc entry points for three allocation sites that
are not simulation state).

This module covers windows 1 and 2. The **extras** are not in the learned set at this commit: they are compared with their
newest ring value in every capture and copied back whole in every load (the cost measured in issue #101).

## 5. Where state lives, and who restores it

| State | Where | Saved by | Restored by |
|---|---|---|---|
| game heap (window 1), image / modules (window 2) | process memory | `r1_ring_capture` (learned pages) | `r1_ring_load` |
| extras (stack, TLS, DLL data, ...) | process memory | every capture (compare all) | every load (all) |
| main's registers | `Ctx` | slot | load |
| other threads | held at a waiting call | slot (place + registers) | `ra_hold_restore`, `restore_thread_regs` |
| fibers (job system) | host fibers' stacks | `fibers_save` | `fibers_restore_all` |
| command buffers being recorded | `nvn_gl.c` save area | `rg_cb_capture` | `rg_cb_load` |
| audio: queue of released buffers | host, `nnaudio.c` | not saved | not restored |
| pads / input queues | `rb_input.c` | not part of the ring | rebuilt from the queues |

The audio row is my reading of `nnaudio.c` (the list of released buffers, `g_released`, is host state). A game thread
(`nus3::audio update`) that never reached a wait again after many forced rollbacks with a real audio device, and did not
with `SSBU_AUDIO_MUTE=1`, was seen once each (PR #102); that this row is the cause is a guess, not shown.

## 6. Reading order

1. `rb.h` and `tests/rb_test.c`: the netcode, small and clear.
2. `rb_rollback.c`, `rbg_rollback` and `rbg_step_body`: what happens at a safe point.
3. The header comment of `r1_ring.c`, then `r1_ring_capture` and `r1_ring_load`.
4. The header comment of `rb_learned.c`, then `ls_veh`, `ls_open`, `ls_capture`.
5. `r1_state.c`: the banners in section 4.4, only as far as the part you are changing.

## 7. Things that make the code hard to follow (for the refactor, not criticism of the design)

- `.c` files include each other; everything is `static` and shared, so what one function may change is not visible
  from its signature (the many `g_*` globals).
- The rollback shares files with older experiments (`SSBU_R1_*`, run-ahead) and with its own test drivers
  (`SSBU_RB_SIM`, `SSBU_RB_NET_TEST_*`).
- Names carry the history (`R1`, `R2`, `R3`, `R4`, `hs12`, `pub1`, `net3`, `lwr`) rather than the role.
- Several file headers point to `plan\phases\*.md`; the `plan` folder of this repository only has
  `LINUX_MAC_INSTALL_SPEED.md`, so those documents are not available to a reader.
- Long functions (the capture, the load and `rbg_step_body` are 90 to 180 lines).

## 8. What was not checked

- Only the headers, function lists and the flow of the main functions were read; the bodies of `fibers_*`, `ra_*`
  and `rbg_step_body` were not read line by line. The table in 4.4 is from the banners, not from a call-graph.
- Whether `tests/rb_test.c` is run by any script (a search of the scripts and docs found no reference).
- Nothing here was run for this document.
