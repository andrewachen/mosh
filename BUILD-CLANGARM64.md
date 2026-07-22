# Mosh CLANGARM64 build spike

This log records the Milestone 0 native-Windows ARM64 build spike. The target is
`aarch64-w64-mingw32` using the UCRT-family MinGW runtime; it is not Cygwin,
WSL, or MSYS.

## Result

`make -f win32/Makefile.dll ARM_MCPU= check` succeeds in the `mosh-arm64`
Docker image. It regenerates the protocol sources and archives all five engine
libraries:

- `src/crypto/libmoshcrypto.a`
- `src/protobufs/libmoshprotos.a`
- `src/util/libmoshutil.a`
- `src/terminal/libmoshterminal.a`
- `src/statesync/libmoshstatesync.a`

`mosh.dll` links from a one-symbol spike body that calls `base64_encode()` from
`libmoshcrypto.a` and constructs a generated `ClientBuffers::UserMessage` from
`libmoshprotos.a`. The successful link uses `-static-libstdc++ -static-libgcc`.
`aarch64-w64-mingw32-nm -u mosh.dll` reports no unresolved C++ runtime symbols
(`__cxa`, `_Z`, `_Unwind`, `_Zn`, or `_Zdl`); `objdump -p` lists only
`msvcrt.dll` and `KERNEL32.dll` as DLL imports.

`libmoshnetwork.a` is intentionally not attempted. Its POSIX socket and
networking implementation needs the M1 WinSock port.

## Toolchain and dependencies

The M0a Docker image provides:

| Component | Observed version / location |
| --- | --- |
| C++ compiler | `aarch64-w64-mingw32-clang++`, clang 14.0.0, target `aarch64-w64-windows-gnu` |
| Protocol compiler | `protoc 3.21.12` (`/opt/protobuf-host/bin`) |
| Protocol runtime | protobuf 3.21.12 (`/opt/mosh-arm64/lib/libprotobuf.a`) |
| OpenSSL target headers and library | 3.0.16 |
| zlib | 1.3.1 |
| terminal database | `/opt/mosh-arm64/lib/libtinfo.a` |

The current MSYS2 CLANGARM64 repository contains the CI package names
`mingw-w64-clang-aarch64-{openssl,protobuf,ncurses,zlib}`. The CI workflow
installs exactly those packages plus `mingw-w64-clang-aarch64-clang` and
`make`.

The direct build uses:

```text
-I/opt/mosh-arm64/include -L/opt/mosh-arm64/lib
-lcrypto -lprotobuf -lz -ltinfo -lws2_32 -luser32 -DNCURSES_STATIC
```

`protobuf.pc` advertises only `-lprotobuf`; `-lz` is therefore explicit. It is
also required by the mosh compressor code. The host `protoc` and target runtime
both report 3.21.12.

The local image's clang 14 rejects `-mcpu=oryon-1`. `win32/Makefile.dll` has
`ARM_MCPU ?= -mcpu=oryon-1`; local invocations use `ARM_MCPU=`. CI can retain
the default when its clang supports Oryon tuning.

M0a version-policy note: local protobuf is 3.21.12, while CI's MSYS2
CLANGARM64 environment uses its own package version. Each environment
regenerates `*.pb.{h,cc}` from the checked-in `.proto` inputs, so a protobuf
major-version skew is tolerable provided each generated source is built against
its matching runtime.

## Configuration

`win32/config.h.clangarm64` is copied to ignored
`src/include/config.h` by the standalone makefile before compilation. Its
resolved feature decisions are:

- Defined: `HAVE_CURSES_H`, `HAVE_GETTIMEOFDAY`, `HAVE_STD_SHARED_PTR`,
  `HAVE_UNISTD_H`, `HAVE_WCHAR_H`, `HAVE_WCTYPE_H`, and `USE_OPENSSL_AES`.
- Defined with a zero value because the source uses `#if`: the byte-order and
  compiler intrinsic declarations (`HAVE_DECL_BE64TOH`, `HAVE_DECL_BETOH64`,
  `HAVE_DECL_BSWAP64`, `HAVE_DECL___BUILTIN_BSWAP64`,
  `HAVE_DECL___BUILTIN_CTZ`, and `HAVE_DECL_FFS`).
- Left undefined: `HAVE_PTY_H`, `HAVE_IP_MTU_DISCOVER`,
  `HAVE_IP_RECVTOS`, `HAVE_PLEDGE`, `HAVE_CLOCK_GETTIME`,
  `HAVE_FORKPTY`, `HAVE_CFMAKERAW`, `HAVE_GETENTROPY`, `HAVE_GETRANDOM`,
  `HAVE_MACH_ABSOLUTE_TIME`, `HAVE_POSIX_MEMALIGN`, `HAVE_PSELECT`,
  `HAVE_SYS_ENDIAN_H`, `HAVE_SYS_RANDOM_H`, and `HAVE_SYS_UIO_H`.

## Windows adaptations discovered

These are deliberately narrow build-enabling adaptations. No crypto protocol,
replay guard, or direction-check logic was changed.

| File and line | Adaptation | Reason |
| --- | --- | --- |
| `src/crypto/crypto.cc:40` | Guarded `<sys/resource.h>` with `#ifndef _WIN32`. | The target has no `sys/resource.h`. |
| `src/crypto/crypto.cc:286` | Made core-dump limit save/restore a Windows no-op. | Win32 has no `getrlimit` / `setrlimit` / `RLIMIT_CORE`; Windows crash-dump policy is not controlled through that POSIX interface. |
| `src/util/locale_utils.cc:43` | Includes `windows.h` only for Windows. | Supplies the locale and environment APIs below. |
| `src/util/locale_utils.cc:80` | Uses `GetLocaleInfoA(LOCALE_IDEFAULTANSICODEPAGE)` to identify code page 65001 as UTF-8. | The target lacks `langinfo.h`, `CODESET`, and `nl_langinfo`. |
| `src/util/locale_utils.cc:131` | Uses `SetEnvironmentVariableA(name, NULL)` to clear locale variables. | The target does not declare POSIX `unsetenv`. |
| `src/util/select.h:42` | Includes `win32/posix_compat.h` under `_WIN32`. | The target has Winsock `fd_set` but no `<sys/select.h>` or POSIX signal-set API. |
| `win32/posix_compat.h:1` | Supplies narrow `sigset_t` / `sigaction` no-op declarations around Winsock `select`. | Lets the utility archive compile; M1 must replace the event-loop signal model with Windows behavior. |
| `src/util/pty_compat.cc:35` | Compiles POSIX pseudo-terminal fallback only outside Windows. | The target lacks `<sys/stropts.h>` and `<termios.h>`; mintty supplies the Windows terminal/PTY integration. |
| `src/terminal/terminal.cc:41` | Supplies a BMP-only Windows `wcwidth` fallback. | The target CRT has no `wcwidth`; supplementary-plane width handling remains a later Windows-terminal fidelity item. |

## Timestamp source verification

The target compiler successfully compiled direct probes for both
`clock_gettime(CLOCK_MONOTONIC, ...)` and `gettimeofday(...)`. Mosh's existing
timestamp source is nevertheless configured with `HAVE_CLOCK_GETTIME` undefined
for this spike: `src/util/timestamp.cc:113` therefore selects the tested
`gettimeofday` fallback. This makes the archive build without a timestamp shim,
but it is not monotonic. M1 must make the final timestamp decision: use the
available MinGW `clock_gettime(CLOCK_MONOTONIC)` after an executable runtime
probe, or add a `QueryPerformanceCounter` / `GetTickCount64` shim if that API
is not reliable in the MSYS2 CLANGARM64 runtime.

## Reproduction

From the wsltty repository, run:

```sh
./build-mosh-arm64-local.sh
```

It runs the M0a dependency smoke test first, then invokes:

```sh
make -f win32/Makefile.dll clean
make -f win32/Makefile.dll CXX=aarch64-w64-mingw32-clang++ AR=aarch64-w64-mingw32-ar ARM_MCPU= check
```

inside the mounted mosh checkout. The check verifies the five archives, the
DLL, its exported `mosh_spike_ok` symbol, and absence of unresolved C++ runtime
symbols.
