# Shared Worlds — save compression benchmark (2026-09-29)

## Context

Satisfactory `.sav` files already use **zlib-compressed chunks** on disk
(`SaveFile.h`). Outer compression (zstd/zlib) was benchmarked before enabling
it for cloud objects.

## Samples

| Save | Raw size |
|------|----------|
| SharedWorld_test-test-autosave-0-ddbb7f.sav (live) | 150.6 KB |
| test_autosave_1.sav (live) | 121.6 KB |
| cheatogame backup pre-download-r1.sav | 157.5 KB |
| valid_random_multichunk.sav (conformance) | 293.4 KB |

Synthetic patterned bodies (via `BuildSynthetic`) compress extremely well but
are **not** representative of real game saves.

## Results (real saves)

### SharedWorld_test… (150.6 KB)

```
zstd-1:  149.9 KB  99.6%  compress 7ms  decompress 8ms
zstd-3:  149.7 KB  99.4%  compress 7ms  decompress 7ms
zstd-5:  149.5 KB  99.3%  compress 10ms decompress 7ms
zstd-10: 149.5 KB  99.3%  compress 16ms decompress 8ms
zlib-6:  148.0 KB  98.3%  compress 12ms decompress 6ms
```

### Other live / conformance saves

- Outer zstd/zlib typically leaves **98–100% of raw size** (gain **0–2%**).
- Decompress stays ~6–8ms at these sizes.
- Higher zstd levels (10) add CPU with negligible size benefit.

## Decision

| Setting | Value | Why |
|---------|-------|-----|
| Algorithm | **zstd** (when gain ≥ 5%) | Fast, good API, vendored |
| Default level | **3** | Fast path; level 10 not worth it |
| Min ratio gain | **5%** | Real saves fall below → store **raw** |
| Fallback | `compression=none` | Explicit metadata; legacy Get still works |

**Production behaviour:** real Satisfactory saves almost always skip outer
compression and upload the raw `.sav` under the uncompressed SHA-256 object id.
The SWOB package path remains for future compressible payloads and tests.

## Storage architecture (unchanged fencing)

- Metadata: Git / folder mini-git (`IWorldRepository`) — JSON only.
- Blobs: `IObjectStore` (GitHub **Release assets**, not Git LFS / not Git blobs).
- Decorator: `EncodingObjectStore` — pack/unpack + poor-ratio skip.
- `RevisionMeta.object` = SHA-256 of **uncompressed** `.sav`.
- Optional `RevisionMeta.saveObject` encoding / future provider fields.

## Retention / GC

- Keep newest N cloud revisions (min 5, default 5).
- Also keep latest `final` / `migration` / `recovered` / `restore` / `import`.
- Object GC: delete listed objects not referenced by retained revisions
  (after metadata prune). Grace-period tombstones: not yet; safe because prune
  runs under lease fence after successful commit.

## Re-run

```bat
build\Debug\sw_compress_bench.exe path\to\save.sav
```

Env: `SW_BENCH_SAVES=a.sav;b.sav`
