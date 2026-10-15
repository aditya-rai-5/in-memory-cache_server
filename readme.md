# In-Memory Cache Server

A high-performance, single-threaded, `epoll`-based in-memory caching server written from scratch in modern C++ (C++17). This project acts as a lightweight, lightning-fast clone of Redis, featuring a custom binary framing protocol, thread-safe memory storage, and automatic TTL (Time-To-Live) expiry mechanisms.

## 🚀 Features

* **High Performance Networking**: Utilizes Linux `epoll` for highly concurrent, non-blocking I/O multiplexing.
* **Socket Failure Handling**: Checks epoll registration failures, closes clients on socket errors and hangups, drains replies after a write half-close, and suppresses SIGPIPE per socket send. Fatal epoll creation/wait or listener failures stop the server.
* **Custom Wire Protocol**: Implements a length-prefixed TCP binary framing protocol to prevent TCP stream fragmentation issues.
* **Thread-Safe Storage**: Core caching engine is protected for concurrent access, allowing future expansion into multi-threaded executors.
* **Active & Lazy Expiry**: Keys with a TTL are lazily deleted on access, and a background sweeper thread runs asynchronously to actively evict expired keys to prevent memory leaks.
* **Case-Insensitive Parser**: Full tokenization and parsing of client commands.
* **Zero External Dependencies**: Built entirely using the C++ standard library and Linux system calls.

## 🏗️ Architecture

The server is built in distinct, decoupled layers:
1. **Network Layer (`src/net`)**: Handles the raw TCP sockets, non-blocking I/O buffers, and the `epoll` Event Loop.
2. **Protocol Layer (`src/protocol`)**: Tokenizes raw byte payloads into structured `Command` objects and dispatches them.
3. **Storage Engine (`src/cache`)**: The `CacheStore` singleton that manages the internal `std::unordered_map`, timestamps, and thread safety.
4. **Common Utilities (`src/common`)**: Thread-safe asynchronous file logging and configuration file parsing.

## 💻 Supported Commands

The server currently supports the following commands (case-insensitive):

| Command | Usage | Description |
|---|---|---|
| **PING** | `PING` | Returns `PONG`. Useful for testing connections. |
| **SET** | `SET <key> <value>` | Stores the value. |
| **SET EX** | `SET <key> <value> EX <seconds>` | Stores the value with an expiration timer. |
| **GET** | `GET <key>` | Retrieves the value for the key. Returns `(nil)` if not found. |
| **DEL** | `DEL <key>` | Deletes the key. Returns `1` if deleted, `0` if it didn't exist. |
| **EXISTS** | `EXISTS <key>` | Returns `1` if the key exists, `0` otherwise. |
| **TTL** | `TTL <key>` | Returns the remaining time to live in seconds. Returns `-1` if no expiry, `-2` if key doesn't exist. |

Command argument counts are strict: `PING` takes no arguments, `SET` takes
exactly a key and value or a key, value, `EX`, and seconds, and the other
commands each take exactly one key. Missing or extra arguments return `ERR`.
Expiry must be a positive base-10 integer written with digits only (leading
zeros are allowed). Suffixes such as `5abc`, decimal values, and a leading `+`
are rejected. Expiries that cannot fit the steady clock's duration or resulting
timestamp are also rejected. A rejected `SET` preserves the existing value and
expiry.

## 🛠️ Build and Run

### Prerequisites
* Linux or Windows Subsystem for Linux (WSL)
* `g++` compiler with C++17 support
* `make`
* `python3` (for integration tests only)

### Building the Server

Compile the project using the provided Makefile:
```bash
make re
```
This will generate the executable inside the `bin/` directory.

### Configuration
The server requires a readable `server.conf` in its working directory. Omitted
settings use the defaults shown below:
```ini
server.port = 6379
log.level = DEBUG
log.file = logs/server.log
log.max_size = 10485760
log.max_files = 3
```

* `server.port`: decimal integer from 1 through 65535; zero is rejected.
* `log.level`: minimum emitted severity, exactly `TRACE`, `DEBUG`, `INFO`,
  `WARN`, `ERROR`, or `FATAL`.
* `log.file`: nonempty filename or path; parent directories are created if needed.
* `log.max_size`: rotation threshold in bytes, from 1 through 2147483647.
* `log.max_files`: retained numbered backups, from 1 through 1000. The upper bound
  limits filesystem work during rotation.

Numeric values must be complete decimal integers: leading zeros are allowed;
signs, suffixes, fractions, and overflow are rejected. Blank lines and full-line
`#` comments are supported; inline comments and quoted values are not interpreted.
Unknown keys, duplicate keys, empty values, and malformed lines stop startup with
a file/line/setting diagnostic on stderr and a nonzero exit status. Invalid
configuration is rejected before logs or sockets are created. The unused
`server.workers` setting has been removed: delete it from older configurations;
the network event loop remains single-threaded.

### Running the Server
```bash
./bin/redis_like
```

## 🧪 Testing

The repository includes C++ tests for configuration, logger thresholds, the cache,
command validation, and network failure handling. Configuration startup tests
launch isolated server processes with temporary files and ports; the command
integration suite uses an already running server. Network tests use real local sockets and deterministic syscall fault injection, including a subprocess with default SIGPIPE handling.

**1. Run C++ Tests and Self-Contained Configuration Startup Tests:**
```bash
make test
```

To run just configuration, logging, and startup regression tests (including a
server build):
```bash
make test-config
```

**2. Run Python Integration Suite:**
Make sure the server is running (`./bin/redis_like`), then open a new terminal and run:
```bash
python3 test/test_server.py
```

## 🤝 Contributing

Contributions are welcome! Check the **Issues** tab for open tasks. 
