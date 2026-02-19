# Building MeshWave

> Step-by-step build instructions for all supported platforms.

---

## Prerequisites

| Requirement | Minimum Version | Check Command |
|-------------|-----------------|---------------|
| C/C++ compiler | GCC 11+ or Clang 14+ | `gcc --version` or `clang --version` |
| CMake | 3.20 | `cmake --version` |
| Python 3 | 3.8+ | `python3 --version` |
| Make | Any | `make --version` |

> **Note:** Windows is not supported. MeshWave uses POSIX-only APIs (`sys/socket.h`, `pthread`, `ifaddrs.h`, `pwrite`, `fcntl`). Windows users can run MeshWave inside **WSL2** — see [Platform Note](#platform-note) at the bottom of this page.

### Platform-Specific Setup

**macOS (Homebrew):**
```bash
xcode-select --install      # includes clang, make
brew install cmake python3
```

**Ubuntu / Debian:**
```bash
sudo apt update
sudo apt install build-essential cmake python3
```

**Fedora / RHEL:**
```bash
sudo dnf install gcc gcc-c++ cmake python3
```

---

## Standard Build

```bash
# Clone the repository
git clone https://github.com/mathewthomas/meshwave.git
cd meshwave

# Create build directory (out-of-source build)
mkdir build && cd build

# Configure
cmake ..

# Compile
make -j$(nproc)
```

This produces a single binary: `build/meshwave`

### Build Output

```
[ 11%] Embedding index.html into web_bundle.h
[ 22%] Building CXX object src/main.cpp.o
[ 33%] Building C object src/server.c.o
[ 44%] Building C object src/client.c.o
[ 55%] Building C object src/discovery.c.o
[ 66%] Building C object src/transfer.c.o
[ 77%] Building CXX object src/http.cpp.o
[ 88%] Building C object src/util.c.o
[100%] Linking CXX executable meshwave
```

---

## Running

### Interactive Mode (Default)

```bash
./meshwave
```

Opens your default browser to `http://localhost:5558`. The dashboard prompts you to choose **Server** or **Client** mode.

### Server Mode (Direct)

```bash
./meshwave --server
```

Starts as a server immediately. The browser still opens for monitoring connected peers and chatting.

### Client Mode (Direct)

```bash
./meshwave --client 192.168.1.42
```

Connects directly to the server at the given IP address. You'll be prompted for a username in the dashboard.

---

## Build Options

### Debug Build

```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
```

Enables debug symbols (`-g`) and disables optimizations for use with `gdb` or `lldb`.

### Clean Rebuild

```bash
# From the build directory
rm -rf *
cmake ..
make -j$(nproc)
```

Or simply delete the `build/` directory and start fresh.

### Verbose Build

```bash
make VERBOSE=1
```

Shows the full compiler commands for each file.

---

## Project Layout for Build

```
meshwave/
├── CMakeLists.txt           ← Build configuration
├── scripts/
│   └── embed_html.py        ← Runs at build time (HTML → C header)
├── src/
│   ├── *.c, *.cpp, *.h      ← Source files
├── web/
│   └── index.html           ← Frontend (embedded into binary)
└── build/                   ← Created by you (out-of-source)
    ├── web_bundle.h          ← Generated: HTML as C string
    └── meshwave              ← Final binary
```

### How HTML Embedding Works

1. CMake invokes `scripts/embed_html.py` as a custom build step
2. The script reads `web/index.html` and produces `build/web_bundle.h`
3. `web_bundle.h` contains: `static const char index_html[] = "...";`
4. `http.cpp` includes this header and serves the string on `GET /`
5. If you edit `web/index.html`, the next `make` automatically re-embeds it

---

## Troubleshooting

### CMake version too old

```
CMake Error at CMakeLists.txt:1:
  CMake 3.20 or higher is required.
```

**Fix:** Update CMake. On macOS: `brew upgrade cmake`. On Ubuntu: install from [cmake.org](https://cmake.org/download/) or use `snap install cmake`.

### Python 3 not found

```
Could not find Python3
```

**Fix:** Ensure `python3` is in your PATH. The embed script requires Python 3 but uses no external packages.

### Port already in use

```
[ERROR] http: bind failed on port 5558
```

**Fix:** Another instance of MeshWave (or another service) is using port 5558. Kill it with:
```bash
lsof -i :5558 | grep LISTEN
kill <PID>
```

### Browser doesn't open

On headless systems or SSH sessions, the auto-open may fail. Manually navigate to `http://<machine-ip>:5558` from any browser on the same network.

---

## Network Requirements

Ensure these ports are open on your firewall:

| Port | Protocol | Direction | Purpose |
|------|----------|-----------|---------|
| 5556 | UDP | Bidirectional | Discovery broadcast |
| 5557 | TCP | Bidirectional | Chat + file data |
| 5558 | TCP | Inbound | HTTP dashboard |

On macOS, you may see a firewall prompt on first run — click **Allow**.

---

## Platform Note

MeshWave targets **macOS** and **Linux** only. It is not supported on Windows natively.

The codebase relies on POSIX APIs that have no direct Windows equivalent:

| POSIX API | Used For | Windows Equivalent |
|-----------|----------|---------------------|
| `sys/socket.h`, `arpa/inet.h` | TCP/UDP networking | `winsock2.h` (different API semantics) |
| `pthread.h` | Threading, mutexes | Win32 threads or C11 `thrd_t` |
| `ifaddrs.h`, `getifaddrs()` | Local IP detection | `GetAdaptersAddresses()` |
| `pwrite()` | Positional file writes | `_lseeki64()` + `_write()` |
| `fcntl()` | Non-blocking sockets | `ioctlsocket()` |
| `select()` with fd sets | I/O multiplexing | `WSAPoll()` or IOCP |
| `__attribute__((packed))` | Wire protocol structs | `#pragma pack(push, 1)` |

Porting would require rewriting the networking, threading, and I/O layers — effectively the entire backend. This is a deliberate design choice: the project is a systems programming exercise focused on Unix socket fundamentals.

### WSL2 Workaround

Windows users can run MeshWave inside **WSL2** with no code changes:

```powershell
# Install WSL2 (one-time, from PowerShell as Administrator)
wsl --install -d Ubuntu

# Inside WSL2
sudo apt update && sudo apt install build-essential cmake python3
git clone https://github.com/M-Chethipuzha/MeshWave.git
cd MeshWave && mkdir build && cd build
cmake .. && make -j$(nproc)
./meshwave
```

Access the dashboard from Windows at `http://localhost:5558`. LAN discovery works if WSL2 is in bridged networking mode.
