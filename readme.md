# In-Memory Cache Server

A high-performance, single-threaded, `epoll`-based in-memory caching server written from scratch in modern C++ (C++17). This project acts as a lightweight, lightning-fast clone of Redis, featuring a custom binary framing protocol, thread-safe memory storage, and automatic TTL (Time-To-Live) expiry mechanisms.

## 🚀 Features

* **High Performance Networking**: Utilizes Linux `epoll` for highly concurrent, non-blocking I/O multiplexing.
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
The server reads from `server.conf` in the root directory. Example configuration:
```ini
server.port = 6379
log.file = logs/server.log
log.max_size = 10485760
log.max_files = 3
```

### Running the Server
```bash
./bin/redis_like
```

## 🧪 Testing

The repository includes both C++ unit tests for the caching engine and a full Python integration suite for end-to-end network testing.

**1. Run C++ Unit Tests (Cache Engine):**
```bash
make test
```

**2. Run Python Integration Suite:**
Make sure the server is running (`./bin/redis_like`), then open a new terminal and run:
```bash
python3 test/test_server.py
```

## 🤝 Contributing

Contributions are welcome! Check the **Issues** tab for open tasks. 
