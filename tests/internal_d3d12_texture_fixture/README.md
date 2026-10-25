# Internal D3D12 texture failure fixture

This offscreen diagnostic fixture tests frame-capture reuse across one failed
texture reload. It does not change rendering defaults or install an SDK.
The normal CMake build does not include the controller or private exports.
Only `build.py` explicitly defines `REX_INTERNAL_D3D12_TEXTURE_FIXTURE` and
links its private controller into a separate GPU module.

The caller includes the installed control's packet vocabulary, shaders,
geometry, runtime helpers and observer, with three guarded observer calls.
The fixture uses the installed headers and public DTOs without changes.

Run from a Windows x64 developer environment with Python 3 and Clang 22.
Use a fresh absolute output directory, an existing Release Ninja Multi-Config
SDK build at the required revision, and its matching installed package. The
builder recompiles
generic texture cache and shared memory, D3D12 texture cache and command
processor, controller and caller. Every production source uses its own
Release compile edge and a unique object name. It reads the existing
compile/link edges, excludes the normal module's PCH, reuses
unmodified plugin objects and libraries, and writes input/output hashes and
the exact commands to `manifest.json`. Builds are sequential.

```powershell
python tests/internal_d3d12_texture_fixture/build.py `
  --sdk $SdkRoot --source $TopicRoot --build $ExistingBuild `
  --install $ExistingInstall --output $FreshBinaryDirectory --clang $Clang
python tests/internal_d3d12_texture_fixture/run.py `
  --binary-dir $FreshBinaryDirectory --output-root $FreshRunDirectory
```

The runner executes eight fresh processes: fault off/on crossed with
ordinary/independent capture, each with the receipt off/on. The private
`REX_TEXTURE_FIXTURE_RECEIPT=0/1` switch controls only the receipt recorder.
Each process uses full resolve readback, synchronous shaders, ROV rendering,
no native window, and its own TEMP/TMP.
Configure and export resolution finish before publishing any ring commands.
The first real A resolve starts a seed epoch and arms frame capture before
the opening swap. The issued seed consumer seals the latest A load in that
epoch. B then resolves over the same guest extent, a target reload can fail
once immediately before scratch allocation, an identical draw repeats, a
disjoint C binding breaks reuse, and the target retries before a disjoint
terminal resolve and closing swap. Only state written by these packet blocks
is replayed when switching producer and consumer.

Required target captures, in occurrence order:

| Fault | Capture mode | Seed / first B use / repeat / retry |
|---|---|---|
| Off | Ordinary | A / B / B / B. Final three share one index |
| Off | Independent | A / B / B / B. Four distinct indices |
| On | Ordinary | A / unavailable / unavailable / B. Retry has a fresh index |
| On | Independent | A / unavailable / unavailable / B. Retry has a fresh index |

Unavailable target uses remain required, with TextureUnresolved, no texture
index or payload, partial draw acquisition and incomplete texture bindings.
The host draw remains issued. Rejection precedes canonical lookup or insertion
and readback allocation or charging. Private receipts check that only the seed
entry and capture exist and their readback budget is unchanged at each rejection.
The successful retry captures B under the same instance and generation recorded
after the failure. The getter relies on capture on the GPU thread after the load
attempt returns. A temporary clean state during preparation does not prove
upload success.

The successful A and B resolve callbacks retain and decode full CPU bytes
before reuse of the guest range. Each comparison checks every logical RGBA8
pixel, excluding D3D12 footprint padding. All assertions remain active under
NDEBUG. After the seed, the target backend count is one with fault off and two
with fault on. The failure ordinal is one relative to the armed reload phase.

Module state belongs solely to the GPU DLL and is bound to one CP/cache/thread.
The final successful frame-end callback copies fixed value receipts and
retires the scope on that GPU thread before owner destruction. The observer's
mutex publishes the copied report to the caller. Callbacks add no fence wait.
Serialization and validation follow existing owned-frame fence completion and
mailbox publication.

`controller.txt` records phase, CP event/frame/submission, complete normalized
key bytes, I/G/resource/dirty state, guest spans, actual resource dimensions,
backend entry/seam/success, capture index/buffer/budget, and view epochs.
Descriptor writes record the real/null choice in the active bindful or
bindless path. Successful UpdateBindings records the inherited epoch, so an
unchanged binding is explicitly joined without inventing a descriptor write.
C and null decisions replace the active epoch as well. Selected use audits
require a real matching resource/key in that epoch. A resource readback alone
does not establish a bound view. Dirty identity audits precede refusal,
and rejected-use receipts retain the real inherited binding epoch. Only the
successful retry passes the selection gate in a fault-enabled run.

Observer IDs and owned-frame/CP IDs are separate namespaces. `join` rows map
ordered issued consumer occurrences using full register files and
frame/submission. Repeated shader hashes and state digests are not unique
identifiers. Non-issued copy wrappers and aggregate acquisition status remain
in the saved snapshot.

`receipt.txt` records the fixed-capacity, value-only write-to-load observations.
It retains actual IssueCopy occurrences, owned copy IDs when available, actual
observer copy IDs, before/after phases and submissions, generic write
declarations, preparation/no-op/attempt/failure/success, invalidations,
actual draw observers and bound views/uses. A generic successful load is
recorded only after the backend returns true. This establishes command
recording. GPU completion and readback require the existing fenced acquisition
of the owned frame. Seed A precedes the owned frame and has no owned copy ID.
Copy IDs and submissions can differ across observation phases.

`check_receipt.py` checks those actual associations against copy callbacks,
owned metadata and selected views/uses. It admits the controlled full base
footprint only. Loss, missing terminal, lifecycle reset or CPU/interference
observations prevent a complete ancestry claim. CPU callback serialization
does not establish the order of CPU stores against GPU reads. The checker
also rejects focused in-memory deletions or corruptions of records for successful
loads, occurrences, and terminal events, and an inserted unknown CPU invalidation.
These controls do not launch additional GPU failure modes.

Each on/off pair compares every retained texture top/subresource, the producer's
tiled and decoded payloads, and final source/output bytes exactly, plus logical
texture/use/layout metadata. Pointer fields and controller binary bytes are
not rendering equivalence criteria. `runs.json` retains native and offline
results for each process. `pairs.json` retains byte comparison counts. The checker
can also run separately with `--capture`, `--fail`, `--independent` and
`--receipt` arguments.

Every run retains raw texture subresources, A/B CPU readbacks, full register
files, PM4 words, fetch words, final swap payloads, controller values, metadata,
hashes and actual loaded module paths. A missing required capture, wrong
pixels/count/identity/epoch, native failure, or module-path mismatch fails
acceptance. Failed output is retained. Choose new directories for a rerun.

For a default-off check, supply `--default-off --baseline-exe $ExistingControl`
to the same builder and a fresh output directory. That route omits both the
fixture define and controller object, and stages the existing baseline
executable unchanged. Run it with its normal output/alpha environment. The
resulting GPU module must omit every `RexTextureFixture*` export.
Both derived module forms recompile all four production sources. The default-off
form omits the private gate and controller. `manifest.json` records source
revision and dirty state, derivation build/install paths, specific compile and
link inputs and commands. A derivation from a development build is not a
package build from the final commit or a substitute for its full CTest/install.
