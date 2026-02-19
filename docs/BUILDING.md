# Building MeshWave

> Step-by-step build instructions for all supported platforms.

---

## Prerequisites

| Requirement | Minimum Version | Check Command |
|-------------|-----------------|---------------|
| C/C++ compiler | GCC 11+ / Clang 14+ / MSVC 19.30+ | `gcc --version` or `cl` |
| CMake | 3.20 | `cmake --version` |
| Python 3 | 3.8+ | `python3 --version` or `python --version` |
| Build tool | Make (Unix) / MSBuild or Ninja (Windows) | `make --version` or `ninja --version` |

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

**Windows (Visual Studio):**

1. Install [Visual Studio 2022](https://visualstudio.microsoft.com/) (Community edition is free)  
   — select the **"Desktop development with C++"** workload during installation
2. Install [CMake](https://cmake.org/download/) (add to PATH during install)
3. Install [Python 3](https://www.python.org/downloads/) (check "Add to PATH" during install)
4. All commands below should be run from the **Developer Command Prompt for VS 2022** or **Developer PowerShell for VS 2022**

Alternatively, using [winget](https://learn.microsoft.com/en-us/windows/package-manager/winget/):
```powershell
winget install Kitware.CMake
winget install Python.Python.3.12
```

**Windows (MinGW / MSYS2):**

1. Install [MSYS2](https://www.msys2.org/)
2. Open the **MSYS2 UCRT64** terminal and run:
```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake python3 make
```

---

## Standard Build (macOS / Linux)

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

## Building on Windows

### Option A: Visual Studio (MSVC)

Open **Developer Command Prompt for VS 2022** or **Developer PowerShell**:

```powershell
# Clone the repository
git clone https://github.com/mathewthomas/meshwave.git
cd meshwave

# Create build directory
mkdir build && cd build

# Configure with Visual Studio generator
cmake .. -G "Visual Studio 17 2022" -A x64

# Build (Release mode)
cmake --build . --config Release
```

The binary is produced at: `build\Release\meshwave.exe`

Alternatively, using **Ninja** for faster builds:
```powershell
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja
```

### Option B: MinGW / MSYS2

Open the **MSYS2 UCRT64** terminal:

```bash
git clone https://github.com/mathewthomas/meshwave.git
cd meshwave
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
mingw32-make -j$(nproc)
```

Produces: `build/meshwave.exe`

### Windows Build Notes

- MeshWave uses POSIX sockets (`<sys/socket.h>`, `<arpa/inet.h>`). On Windows, these are replaced by **Winsock2** (`<winsock2.h>`, `<ws2tcpip.h>`). The source includes platform guards:
  ```c
  #ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32")
  #else
    #include <sys/socket.h>
    #include <arpa/inet.h>
  #endif
  ```
- On Windows, `WSAStartup()` must be called before any socket operation. This is handled in `main.cpp`.
- `pwrite()` is not available on Windows; the transfer module uses `_lseeki64()` + `_write()` as a fallback.
- The `open()` browser command uses `ShellExecute()` on Windows instead of `system("open ...")` or `xdg-open`.

---

## Running

### Interactive Mode (Default)

```bash
# macOS / Linux
./meshwave

# Windows (from build directory)
.\Release\meshwave.exe
```

Opens your default browser to `http://localhost:5558`. The dashboard prompts you to choose **Server** or **Client** mode.

### Server Mode (Direct)

```bash
# macOS / Linux
./meshwave --server

# Windows
.\Release\meshwave.exe --server
```

Starts as a server immediately. The browser still opens for monitoring connected peers and chatting.

### Client Mode (Direct)

```bash
# macOS / Linux
./meshwave --client 192.168.1.42

# Windows
.\Release\meshwave.exe --client 192.168.1.42
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
# macOS / Linux
make VERBOSE=1

# Windows (MSVC)
cmake --build . --config Release --verbose
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
    ├── meshwave              ← Final binary (macOS / Linux)
    └── Release/meshwave.exe  ← Final binary (Windows / MSVC)
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
# macOS / Linux
lsof -i :5558 | grep LISTEN
kill <PID>

# Windows (PowerShell, run as Administrator)
Get-NetTCPConnection -LocalPort 5558 | Select-Object OwningProcess
Stop-Process -Id <PID>
```

### Browser doesn't open

On headless systems or SSH sessions, the auto-open may fail. Manually navigate to `http://<machine-ip>:5558` from any browser on the same network.

### Windows: Winsock initialization failed

```
[ERROR] WSAStartup failed
```

**Fix:** This should not happen on modern Windows. Ensure you are running Windows 7 or later. If it persists, check that the `ws2_32.dll` system library is not corrupted.

### Windows: `cmake` not recognized

```
'cmake' is not recognized as an internal or external command
```

**Fix:** CMake is not in your PATH. Either:
- Re-run the CMake installer and check **"Add CMake to the system PATH"**
- Or use the full path: `"C:\Program Files\CMake\bin\cmake.exe"`

### Windows: `python` not found during build

```
Could not find Python3
```

**Fix:** Ensure Python 3 is installed and in your PATH. On Windows, the command is often `python` (not `python3`). Verify with:
```powershell
python --version
```
If installed via the Microsoft Store, you may need to disable the app alias in **Settings → Apps → App execution aliases**.

### Windows: Linker error `unresolved external symbol`

If you see errors referencing `WSAStartup`, `socket`, `send`, `recv`, etc.:

**Fix:** The Winsock library is not being linked. Ensure `ws2_32.lib` is linked in CMakeLists.txt:
```cmake
if(WIN32)
  target_link_libraries(meshwave PRIVATE ws2_32)
endif()
```

---

## Network Requirements

Ensure these ports are open on your firewall:

| Port | Protocol | Direction | Purpose |
|------|----------|-----------|---------|
| 5556 | UDP | Bidirectional | Discovery broadcast |
| 5557 | TCP | Bidirectional | Chat + file data |
| 5558 | TCP | Inbound | HTTP dashboard |

On macOS, you may see a firewall prompt on first run — click **Allow**.

On Windows, you will see a **Windows Defender Firewall** prompt on first run — click **"Allow access"** for both private and public networks. If you missed the prompt, add the rules manually:

```powershell
# Run as Administrator
New-NetFirewallRule -DisplayName "MeshWave UDP Discovery" -Direction Inbound -Protocol UDP -LocalPort 5556 -Action Allow
New-NetFirewallRule -DisplayName "MeshWave TCP Data" -Direction Inbound -Protocol TCP -LocalPort 5557 -Action Allow
New-NetFirewallRule -DisplayName "MeshWave HTTP Dashboard" -Direction Inbound -Protocol TCP -LocalPort 5558 -Action Allow
```
