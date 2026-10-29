# cpp-systems-programming

Systems-programming modules in C, built and verified on Linux.

Each module is a self-contained library with a public header, a test suite, and its own
build target — not a single-file exercise.

## Modules

| Module | What it covers | Public interface | Verification |
|---|---|---|---|
| [`concurrent-lock`](src/concurrent-lock) | Five ways to increment a shared counter: unsynchronised, mutex, spinlock, `__atomic` builtins, and x86 inline assembly | `counter.h` | 68 checks · ASan/UBSan · TSan |
| [`thread-pool`](src/thread-pool) | Fixed-size pool with an FIFO task queue, condition-variable handoff, a drain-on-destroy shutdown, and optional bounded queues with reject-or-block backpressure | `threadpool.h` | 109 checks · 30 end-to-end · ASan/UBSan · TSan |
| [`mysql-client`](src/mysql-client) | MySQL C API behind an opaque handle: buffered and streaming result sets, prepared statements, BLOB round-trip | `mysqlclient.h` | 119 checks · 41 end-to-end · 50 live checks · 24 streaming checks · 1M-row memory comparison against MySQL 8.0 · ASan/UBSan |
| [`contact-list`](src/contact-list) | Contact book on a doubly linked list plus a hash index that grows and shrinks with the load factor; text format that round-trips byte for byte | `contacts.h` | 209 checks · 466 allocation-failure injections · 30 end-to-end · ASan/UBSan |
| [`word-count`](src/word-count) | Streaming byte scanner for lines, words, and bytes, with a `-j` parallel driver built on `thread-pool` | `wordcount.h` | 40 checks · 28 parallel-vs-sequential comparisons · cross-checked against `wc` · ASan/UBSan · TSan |

All five modules are libraries: the CLI links against `lib*.a` and never
recompiles the module sources, so "this module can be linked independently" is
re-verified on every build. `mysql-client` goes one step further and keeps
`<mysql.h>` out of its public header, so callers are not tied to a client-library
version; `mysql-client/tests/test_pure` links with no MySQL dependency at all and
runs on machines that have no client library installed.

`word-count` is also the one case where two modules are wired together: its CLI driver
submits one task per file to `thread-pool`, while `libwordcount.a` itself keeps zero
references to it — so a caller that only wants to count a file still needs neither
pthread nor a worker pool.

## Layout

```
.
├── src/                  one directory per module
│   ├── concurrent-lock/
│   ├── thread-pool/
│   ├── mysql-client/
│   ├── contact-list/
│   └── word-count/
├── docs/                 design notes and diagrams
├── CHANGELOG.md          day-by-day work log
├── Makefile              builds every module that has one
├── LICENSE
└── README.md
```

A module that has been made into a library follows this layout:

```
src/<module>/
├── include/<module>.h    public interface
├── src/                  implementation + CLI driver
├── tests/                unit tests and end-to-end checks
├── testdata/             input data whose byte count is documented behaviour
├── Makefile              all / lib / cli / test / check / sanitize / bench / clean
└── README.md             deep technical notes (in Chinese)
```

## Quick start

```bash
git clone https://github.com/joker2025350/cpp-systems-programming.git
cd cpp-systems-programming

make                    # build every module that has a Makefile
make test               # run every test suite

# or work on one module
cd src/word-count
make && make test && make check
```

`mysql-client` needs `libmysqlclient-dev`. Without it, either point the build at an
unpacked copy of the packages (no root required — see
[its README](src/mysql-client/README.md#41-外部依赖libmysqlclient)) or skip it:

```bash
make SKIP=mysql-client
```

Per-module targets:

| Target | Purpose |
|---|---|
| `make` | build the static library and the CLI |
| `make test` | unit tests |
| `make check` | end-to-end checks (where present) |
| `make check-live` | checks that need a running MySQL server (`mysql-client` only) |
| `make check-scale` | 1M-row peak-memory comparison, buffered vs streaming (`mysql-client` only) |
| `make sanitize` | rebuild with AddressSanitizer + UndefinedBehaviorSanitizer and run the tests |
| `make tsan` | rebuild with ThreadSanitizer (`concurrent-lock`, `thread-pool`, `word-count`) |
| `make bench` | throughput measurements |
| `make clean` | remove `bin/` |

## Continuous integration

`.github/workflows/ci.yml` runs on `ubuntu-22.04` (gcc 11.4.0 — the same compiler the
module READMEs quote) on every push: default build, unit tests, end-to-end checks,
ASan+UBSan, TSan, benchmarks, and a strict-warning build. Zero warnings and a clean
sanitizer run are hard conditions. `mysql-client` is excluded via `SKIP=mysql-client`,
since it needs the client headers and a live server.

The job ends by writing its summary — per-stage warning counts, per-module check
counts, benchmark tables — to the `ci-reports` branch, one file per commit SHA. That
branch exists because Actions logs need credentials to read while repository content
does not, so `ci-reports` is the part of the build record anyone can inspect.

`-Wcast-qual` is counted but not enforced. The `const` query functions in `counter` and
`threadpool` have to lock a synchronisation primitive, and `pthread_mutex_lock` takes a
non-`const` pointer. C11 6.7.3p6 makes casting away `const` well-defined as long as the
object itself is not `const`-qualified, which holds for objects returned by `create`.
Removing the casts properly means moving the primitives out of the object (C has no
`mutable`) — a structural change, tracked in the module READMEs instead of being folded
into an unrelated commit.

## Requirements

- Linux (the code targets POSIX; `concurrent-lock/asm` additionally requires x86)
- gcc or clang with C11 support
- pthread
- `libmysqlclient-dev` — `mysql-client` only

Nothing here builds on Windows without a POSIX layer. `src/mysql-client` reads
credentials from `config.h` when present (copy `config.h.example`, it is git-ignored),
then from `MC_*` environment variables, then from command-line flags; without
`config.h` it still builds and defaults to a password-less connection on localhost.

## Conventions

- C11 only. Every module compiles clean under `-Wall -Wextra -Wconversion`; a warning is
  treated as a defect, not as noise.
- Public interface in `include/`, implementation in `src/`. Tests link against the built
  library rather than including the sources.
- Behaviour claims in a module README are backed by a command the reader can run. Numbers
  are measured, not estimated, and the environment is stated.
- Text formats must round-trip: whatever the writer produces, the reader must return
  byte for byte.
- Test data whose byte count is part of the documented behaviour is marked `-text` in
  `.gitattributes`, so line-ending conversion cannot quietly change it.

## Notes

- Module READMEs are written in Chinese and contain the full reasoning, defect history,
  and measurements. This top-level README stays in English so the repository is
  readable at a glance.
- `CHANGELOG.md` records what changed and why, one entry per working day.

## License

[MIT](LICENSE)
