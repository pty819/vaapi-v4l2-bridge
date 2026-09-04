# AV1 hot-path cleanup — combined request controls + zero-waste fixes

Date: 2026-09-04 · Status: approved scope (user decisions recorded inline) ·
Baseline commit: `2e59466`

## Context

The post-copy-out audit (`8b5ea0c`) of the AV1 per-frame path found four
items. The decode semantics (refresh flags, slot model, pool policy) are
correct and are NOT touched by this work — this is purely an
io-count/waste cleanup:

1. **Three separate `VIDIOC_S_EXT_CTRLS` per frame** (film grain, frame,
   tile-group entries). `v4l2_ext_controls` carries all of them in one
   call. AV1 steady state is 9 ioctls/frame vs the ~7/frame measured for
   H.264/HEVC in `docs/perf-baseline-2026-09.md`.
2. **Silent truncation at 32 tile params.** `v4l2sl_collect_decode_buffers`
   caps `n_slice_params` at 32 and `av1_translate` clamps `n_tiles`; a
   stream with more tiles decodes wrong with no diagnostics.
3. **Duplicated parse attempt for ffmpeg submissions.** The OBU-truth
   parser is tried on `span` (= `cb.largest`) first, then the fallback
   loop re-tries `all_spans[0]` — the same pointer when there is a single
   slice-data buffer (always, for ffmpeg).
4. **~10 `getenv("V4L2SL_DEBUG")` calls per frame** (translate tail,
   `av1_release_unrefd`, device.c mmap/pop gates, shared with other
   codecs). Each is a linear scan of `environ`.

## Goals

1. AV1 request controls go out in **one** `S_EXT_CTRLS` (9 → 7
   ioctls/frame).
2. `> 32` tile params produce a **loud warning** (decode still proceeds —
   see design rationale).
3. The fallback parse loop **skips the pointer already tried** as `span`.
4. `V4L2SL_DEBUG` is read **once** at driver init into a cached flag; all
   gates test the flag.

## Non-goals / decisions (user-confirmed)

- **No fallback if the kernel rejects combined control submission.** The
  target device either accepts it or the matrix fails loudly at
  acceptance step 2. No per-control degrade path, no extra state.
- **Verification stops at matrix + full-clip SVT** — no browser round this
  time (the change is userspace-side sequencing of identical kernel
  objects; the Chrome-path code is exercised by the same translate
  function the matrix drives).
- No behavior change to refresh parsing, slot model, pools, or the
  trust gate.

## Design

### 1. Combined request controls (`src/v4l2stateless_av1.c`)

In `v4l2sl_av1_translate`, replace the three sequential
`v4l2sl_set_request_controls` calls with one `v4l2_ext_controls` whose
`controls[]` array holds, in this order: FILM_GRAIN, FRAME,
TILE_GROUP_ENTRY (tile group omitted when `n_tiles == 0`, as today —
count is 2 or 3).

Sequencing change that comes with it: all controls are submitted
**before** the output buffer is popped/copied. Today's order is
grain → frame → pop out → memcpy → tile group. Moving the tile-group
control ahead of the pool pop is safe (it references only
`tile_params[]`, never the copied output buffer) and simplifies the error
path: a control failure returns `VA_STATUS_ERROR_OPERATION_FAILED` with
**no pool state to unwind** — the current code has two different
unwinding shapes (frame-ctrl failure returns without push, tile-ctrl
failure pushes `out_buf_idx`) which collapse into one.

### 2. Tile-truncation warning (`src/v4l2stateless_av1.c`)

After the `n_tiles` clamp in `av1_translate`:

```c
if (cb.n_slice_params > 32)
    fprintf(stderr, "v4l2stateless: AV1 %d tile params exceed the "
            "32-entry table; decoding first 32 — picture will be wrong\n",
            cb.n_slice_params);
```

Rationale for warn-and-continue over hard failure: a failed
`vaEndPicture` entrypoint gets cached by Chrome for the whole session
(known session stickiness) and kills AV1 until relaunch; a corrupt frame
recovers at the next IDR. Same philosophy as the existing corrupt-frame
`-2` path.

### 3. Duplicate-parse skip (`src/v4l2stateless_av1.c`)

Fallback loop gains a pointer comparison:

```c
if (all_spans[si] && all_spans[si] != span &&
    av1_parse_hdr_refresh(all_spans[si], all_sizes[si], pic, &parsed))
```

`cb.largest` is by definition one of `all_spans[]`, so at most one
duplicate attempt is removed; zero behavior change otherwise.

### 4. Cached debug flag (`src/v4l2stateless.c`, `src/v4l2stateless.h`, callers)

```c
/* header */ extern int v4l2sl_debug;
/* v4l2sl_init */ v4l2sl_debug = !!getenv("V4L2SL_DEBUG");
```

All `getenv("V4L2SL_DEBUG")` sites are replaced by `v4l2sl_debug` —
mechanically, including init-time prints, so there is exactly one
reading semantic (the env var is read at process start only, which was
already the de-facto contract). Sites live in `v4l2stateless.c`,
`v4l2stateless_av1.c`, `v4l2stateless_device.c`.

## Acceptance (gate, in order)

1. `ninja` clean; install `.so` to `/usr/lib/aarch64-linux-gnu/dri/`.
2. `bash tests/run_full_matrix.sh` **twice** → `MATRIX_ALL_PASS` both
   runs (correctness + determinism; this is also the combined-control
   kernel gate).
3. `av1_svt.mp4` full-clip hw-vs-sw framemd5 → 60/60 bit-exact (the
   matrix's 32-frame blind-spot check).
4. ioctl-count evidence: `strace -f -e trace=ioctl` over a short AV1
   decode, steady-state count per frame **7** (was 9). Not a pass/fail
   gate — it is the measurement that proves goal 1 landed.

## Risks

- **Combined submission rejected by hantro** → visible immediately at
  acceptance step 2 as matrix AV1 failures. Response per user decision:
  no auto-fallback; if it fails, the change is revisited rather than
  patched around.
- **Control-before-copy reordering** — userspace-only sequencing; the
  kernel receives byte-identical objects. Covered by the full matrix.
- **Env-var caching** — a mid-process env toggle was never supported;
  no compatibility surface.
