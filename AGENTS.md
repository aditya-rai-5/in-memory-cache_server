# Repository Guidelines

## Project Structure & Module Organization

`main.cpp` wires configuration, logging, the TTL sweeper, and the event loop. Implementation lives in `src/`, with matching public headers in `include/`: `net/` handles sockets, framing, and epoll; `protocol/` parses and executes commands; `cache/` manages storage and expiry; `common/` provides configuration and logging. Tests live in `test/`. Runtime settings are in `server.conf`; build output goes to ignored `build/` and `bin/` directories, and logs to `logs/`.

## Build, Test, and Development Commands

Use Linux or WSL with C++17-capable `g++`, Make, and Python 3. Run commands from the repository root:

- `make all`: compile `bin/redis_like` with warnings enabled.
- `make re`: clean and rebuild; use after header changes because the makefile does not track header dependencies.
- `make run`: build and start the server, reading `server.conf`.
- `make test`: compile and run cache, command, and network C++ tests.
- `python3 test/test_server.py`: run integration tests with the server already running on `127.0.0.1:6379` in another terminal.
- `make clean`: remove compiled output.

## Coding Style & Naming Conventions

Use C++17 and follow the surrounding file's formatting; newer C++ code generally uses two-space indentation. Preserve existing line endings and avoid unrelated formatting changes. Use `snake_case` for functions, variables, and filenames; `PascalCase` for classes; and trailing underscores for private data members. Headers use `#pragma once`. Python uses four-space indentation. No formatter or linter configuration is checked in; keep compiler warnings clean.

## Testing Guidelines

Tests use standard-library assertions or custom checks, without an external framework. Follow `test/test_<area>.cpp` and descriptive `test_<behavior>` function names. Add regression cases for changed behavior, including invalid arguments, TTL overflow, and preservation of state after rejected writes. Network changes need deterministic syscall-failure checks and TCP integration coverage. No numeric coverage threshold is configured. Run both suites before submitting; CI also builds and runs them.

## Commit & Pull Request Guidelines

History favors short imperative subjects, often prefixed with `feat:`; follow that pattern for features. Keep commits focused. PRs should describe the problem, resulting behavior, and validation commands/results, and link relevant issues. Explain protocol or configuration changes explicitly.

## Protocol & Configuration Notes

Frames use a four-byte big-endian length prefix; clients must implement this custom protocol. Preserve strict argument counts and safe expiry validation. Keep generated binaries and logs out of commits.
