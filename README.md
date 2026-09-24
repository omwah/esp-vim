# vim-tiny-p4

Porting [Vim](https://github.com/vim/vim) to the ESP32-P4, targeting the
[M5Stack Tab5](https://docs.m5stack.com/en/core/Tab5).

The plan, its current status, and the reasoning behind it:

- **[docs/PLAN.md](docs/PLAN.md)** — the living plan, phase by phase
- **[docs/DECISIONS.md](docs/DECISIONS.md)** — why things are the way they are

## Getting set up

```sh
git clone <this repo> && cd vim-tiny-p4
git lfs pull              # fetch the vendored upstream archives
scripts/prepare-deps.sh   # verify, extract and patch into build-deps/
```

`prepare-deps.sh` uses **no network** — everything it needs is in `third_party/` as Git LFS
blobs. It is idempotent: re-running resets `build-deps/` from the archives.

## How third-party source is handled

Upstream source is never committed as files. Each dependency is a **Git LFS archive** in
`third_party/` plus a **patch series** in `patches/<dep>/`, extracted and applied into the
git-ignored `build-deps/` at build time. This keeps the repository reviewable, makes our
changes to upstream explicit, and makes re-syncing to a newer Vim a bounded job.

`third_party/manifest.txt` records each archive's provenance and sha256. Note the sha256
attests the blob *we vendored*, not an upstream-published digest — see the manifest header
for why that distinction is necessary.

To add a dependency: `scripts/add-dep.sh --name NAME --version V --url URL`. It is the only
script that touches the network.

To change upstream source: see *Patch authoring* in [docs/DECISIONS.md](docs/DECISIONS.md).

## Layout

| Path | What |
|---|---|
| `docs/` | plan and decisions |
| `third_party/` | LFS archives + manifest. No source files. |
| `patches/<dep>/` | our changes to upstream, applied in lexical order |
| `scripts/` | dependency prep, image build, emulator harness |
| `esp-vim/` | the ESP-IDF project |
| `build-deps/` | extracted + patched upstream (git-ignored) |
