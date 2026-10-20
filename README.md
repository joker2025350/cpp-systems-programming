# cpp-systems-programming

Systems-programming modules in C, built and verified on Linux.

Each module is a self-contained library with a public header, a test suite, and its own
build target — not a single-file exercise. A module that is still a single file is marked
as such in the table below.

## Modules

| Module | What it covers | Public interface | Verification |
|---|---|---|---|
| [`concurrent-lock`](src/concurrent-lock) | Five ways to increment a shared counter: unsynchronised, mutex, spinlock, `__atomic` builtins, and x86 inline assembly | `counter.h` | 68 checks · ASan/UBSan · TSan |
| [`thread-pool`](src/thread-pool) | Fixed-size thread pool, task queue, condition-variable handoff | — (single file) | ASan |
| [`mysql-client`](src/mysql-client) | MySQL C API: result-set iteration, prepared statements, BLOB transfer | — (single file) | ASan |
| [`contact-list`](src/contact-list) | Contact book on a doubly linked list; text format that round-trips byte for byte | `contacts.h` | 136 checks · 21 end-to-end · ASan/UBSan |
| [`word-count`](src/word-count) | Streaming byte scanner for lines, words, and bytes | `wordcount.h` | 40 checks · cross-checked against `wc` · ASan/UBSan |

The `concurrent-lock`, `contact-list`, and `word-count` modules are libraries: the CLI
links against `lib*.a` and never recompiles the module sources, so "this module can be
linked independently" is re-verified on every build. `thread-pool` and `mysql-client` are
single-file programs for now.

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

Per-module targets:

| Target | Purpose |
|---|---|
| `make` | build the static library and the CLI |
| `make test` | unit tests |
| `make check` | end-to-end checks (where present) |
| `make sanitize` | rebuild with AddressSanitizer + UndefinedBehaviorSanitizer and run the tests |
| `make tsan` | rebuild with ThreadSanitizer (concurrent-lock only) |
| `make bench` | throughput measurements |
| `make clean` | remove `bin/` |

## Requirements

- Linux (the code targets POSIX; `concurrent-lock/asm` additionally requires x86)
- gcc or clang with C11 support
- pthread
- `libmysqlclient-dev` — `mysql-client` only

Nothing here builds on Windows without a POSIX layer. Copy `src/mysql-client/config.h.example`
to `config.h` and fill in your own credentials before building `mysql-client`; `config.h`
is git-ignored.

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
