# Mosh CLANGARM64 build and frontend record

## Current M2 status

`mosh.exe` is now the native Windows console frontend, not the M0 standalone
engine spike. It accepts one destination, `mosh.exe <user@host>`, which spawns
`ssh` and reads the endpoint from the server's `MOSH CONNECT` reply; the CI bare
invocation verifies the well-defined usage exit code `2`. That invocation
returns before it opens a console or constructs `MoshCore`, so it is not
console, core, or crypto runtime evidence. Console/session runtime validation
remains native Windows ARM64 work.

### Known limitations

Three claims about the session key are easy to conflate here, and only the
narrowest of them is tested. The source accepts exactly one argument shape,
`mosh.exe <user@host>`, and nothing in the program reads a session key from
`argv`; the SSH bootstrap carries the key in-process instead, and the `ssh`
command line is fully built before the server has issued a key. CI proves
less: it asserts that the binary it builds refuses an endpoint-shaped
invocation and that the usage text advertises no key or positional endpoint,
which guards the retired spelling rather than establishing that no credential
can reach any command line. Nothing here has been run against a packaged
release artifact. M4 owns that gate, as two separable claims: that the
packaged `mosh.exe` follows the documented resolution rules under controlled
`PATH` layouts and DLL search conditions, and that one known-good OpenSSH
version completes the credential-flow check. No gate can certify an arbitrary
`ssh.exe` a user later puts on `PATH`; that stays an unverified external
dependency by construction. `win32/PARITY.md` records the invariants a broader
check would have to cover. It also disposes of `ssh.exe`, which holds the same
key: a `PATH`-resolved OpenSSH client is a trusted external dependency, with
the residual risk accepted, on the grounds that upstream trusts it more
loosely still.

Say what that buys, in capabilities rather than identities. The adversary S1
answers is one that can *observe* — list processes and read their command
lines, or collect telemetry that does. Against that reader, keeping the key
out of the parameter block is a real gain. Two capabilities are excluded
outright, however they were obtained: reading another process's memory, and
controlling which executable `ssh` resolves to — modifying the resolved binary
itself, or placing one in a writable entry that the search reaches first.
Writing some later `PATH` directory does not qualify; it never wins the match.
Neither exclusion requires the user's full token, since a delegated ACL on one
early directory is enough, so the boundary is the capability rather than how
privileged its holder appears. Administrators and attached debuggers are out
of scope for the same reason. No part of this repair should be read as
defending against any of them.

The supported topology is bounded, and deliberately so, though for two
separate reasons that should not be run together. `mosh.exe` connects to the
address the server reports in `SSH_CONNECTION` and implements no discovery of
its own, so behind NAT or a load balancer that address is the server's private
one and the session times out — `win32/PARITY.md` I9, out of scope by
decision. Separately, the bootstrap pins `ProxyJump` and `ProxyCommand` off,
so a host reachable only through a jump fails at `ssh`. That second
restriction is broader than the first requires: a bastion mandated for TCP/22
can coexist with a directly routable UDP address, and such a session is
refused anyway. It has no recorded justification and is carried as an open
question, `win32/PARITY.md` I10 — refused by the current implementation, with
the policy unresolved rather than decided. Connection sharing is pinned off
too, by `-S none`, and that one is decided: a pre-existing control master
would hold the plaintext session key outside this process's Job Object, which
is not the trust boundary S2 assumes. The remote command also pins
`LC_ALL=C.UTF-8`, which servers lacking that locale — some macOS and BSD hosts —
will reject. And `ssh` is invoked with `-T`, which forces no remote PTY — `-n`
alone would not, since it only redirects this side's stdin and a user's
`RequestTTY=force` would still allocate a terminal — so `mosh-server` must be
new enough to survive a failed initial window-size query: 1.2.4 and older exit,
1.2.5 falls back to 80x24.

The supported case is therefore narrower than "a Linux host": a host the client
can reach directly over UDP at its `SSH_CONNECTION` address, with `C.UTF-8`
available and `mosh-server` 1.2.5 or later, reached over a plain `ssh`
connection with no proxy or jump host. `RequestTTY`, `ProxyJump`,
`ProxyCommand`, and `ControlPath` — the last via `-S none` — are the settings
the bootstrap overrides outright; everything else in a user's SSH
configuration — host aliases, `HostName`, ports, identities, `RemoteCommand`,
`SessionType` — is still honored and can still prevent a bootstrap that this
document does not promise to diagnose. Of what falls outside that matrix, the
discovery gap is out of scope and not scheduled; the proxy refusal is
unresolved and tracked as I10. The retired direct-endpoint form was the workaround
for the reachability half of it, and removing it is worth more than the
topologies it covered.

Crash-dump exposure is untouched by all of that and remains open: the Windows
counterpart of `disable_dumping_core()` is a no-op and is never called, so a
crash while the key is in memory can still persist it through Windows Error
Reporting or any other minidump path. That is a release blocker (see Spike-only
shims below), and `win32/PARITY.md` finding S2 holds its disposition.

## Historical M0 standalone-engine spike record

The remainder of this log records the Milestone 0 native-Windows ARM64 build
spike and its evidence at the historical commits it names. The target is
`aarch64-w64-mingw32` using the UCRT-family MinGW runtime; it is not Cygwin,
WSL, or MSYS.

**Target CPU: Qualcomm Oryon (Snapdragon X-class).** This port intentionally
targets Oryon-class Windows-on-ARM hardware — the makefile default is
`ARM_MCPU ?= -mcpu=oryon-1`, and `-mcpu` may emit ISA that faults on older
Windows-on-ARM parts (e.g. SQ1/SQ2, Ampere). Baseline portability to non-Oryon
ARM64 is an explicit **non-goal** for now (the deliverable is validated on
Snapdragon X hardware at M5). Consequence: this `-mcpu=oryon-1` default must
**not** silently become a general-release build configuration — a
broadly-distributable artifact would first require choosing a conservative
`-march` baseline (with `-mtune=oryon-1` for tuning) and validating on a
non-Oryon environment. See Toolchain for the `ARM_MCPU` override mechanics.

**CI build vs product build.** The hosted `windows-11-arm` runner is Azure
Cobalt 100 (Arm Neoverse N2), **not** Oryon. Executing an `-mcpu=oryon-1` binary
there would be nondiagnostic — a failure could mean a port defect *or* an
Oryon-only instruction the N2 runner lacks, and a pass would only mean the
spike's reached code happened to avoid such an instruction. So the CI gate
**builds a conservative baseline** (`ARM_MCPU=-march=armv8-a -mtune=oryon-1`;
`-mtune` is scheduling-only and does not change the ISA) that genuinely runs on
the Cobalt runner. This makes CI a *diagnostic* native-ARM64 build/link/ABI/
liveness gate: a failure is a real port defect, not an incompatible runner. It
deliberately does **not** validate the `-mcpu=oryon-1` product ISA — that is
owned by **M5** (execution on Snapdragon X hardware). The makefile default
remains `-mcpu=oryon-1` for local and product builds; only CI overrides it.

## Result

**Status: M0 CLOSED — authoritative CI evidence recorded** (run `29928638939`
on `ba642de`; see the evidence table in the import-set section below).
`make -f win32/Makefile.win check` succeeds in the `mosh-arm64` Docker image
(local cross-compile), but that image is an ABI-divergent smoke test (msvcrt,
not the target UCRT — see Toolchain) and is explicitly *not* target-runtime
evidence. The authoritative check is the MSYS2 CLANGARM64 CI job
(`windows-11-arm`) — authoritative for the native-ARM64 build, link, UCRT ABI
boundary, import deny-list, and baseline liveness run, **but not** for Oryon-ISA
correctness (that is M5; see the CI-build note above). That run has now recorded
its durable evidence bundle (SHA, effective flags, full `pacman -Q` closure, PE
machine type, import + undefined-symbol reports, artifact checksum, runtime
command + exit status), so M0's closure contract is satisfied.

*Closure keys to the build inputs, not the literal HEAD SHA.* The authoritative
run is the CI run whose checked-out **build inputs** (sources, makefile,
workflow, config, `.proto` — everything except this document's evidence text)
match this commit. Recording that run's results is an **evidence-only
documentation commit** that changes no build input, so it does not itself
require a fresh run — this avoids the otherwise self-referential loop where
writing the evidence creates a new commit lacking evidence. The recorded
evidence names the CI run's own head SHA and run ID for traceability.

The local build regenerates the protocol sources and archives all five engine
libraries:

- `src/crypto/libmoshcrypto.a`
- `src/protobufs/libmoshprotos.a`
- `src/util/libmoshutil.a`
- `src/terminal/libmoshterminal.a`
- `src/statesync/libmoshstatesync.a`

At the historical M0 commit, `mosh.exe` linked from a standalone-exe spike body
that referenced one entry point from each archive:

- an AES-OCB `Crypto::Session` encrypt/decrypt round-trip from `libmoshcrypto.a`
  (also pulls the OpenSSL cipher path — see below)
- `ClientBuffers::UserMessage` from `libmoshprotos.a`
- `freeze_timestamp()` / `frozen_timestamp()` from `libmoshutil.a`
- `Terminal::Framebuffer` constructor from `libmoshterminal.a`
- `Terminal::Complete::wait_time()` out-of-line method from `libmoshstatesync.a`

The C++ runtime and protobuf (with Abseil/utf8 closure on CI) are statically
linked into `mosh.exe` so the binary does not depend on those runtime DLLs
being on PATH. The exe MAY dynamically import the C libraries it needs
(libcrypto, tinfo/ncurses, zlib), which are bundled alongside the exe in the
distribution (NOT "provided by a host runtime").

### Scope of the M0 claim

What M0 establishes: all five engine `.cc` sets **compile** into archives under
clang++ CLANGARM64, and a spike that references a **representative entry point
from each archive** links into a native ARM64 `mosh.exe` that carries **no
dynamic import of any deny-listed C++-runtime or protobuf/Abseil DLL** (strong
evidence the static-fold worked — the single most important thing this spike
exists to catch), and runs to a clean exit inside the provisioned MSYS2
CLANGARM64 environment. Precisely: the gate is a **deny-list**, so it proves the
absence of the named C++-runtime and protobuf/Abseil DLLs (see the check target
for the exact list, which includes the Abseil `utf8` helpers), not the positive
statement that every remaining import is approved. A positive direct-import
allowlist is deferred to M4 (packaging) per the scope decision below. Because static
archive members link lazily, this proves the referenced closure links — not
that every unreferenced member (e.g. `statesync/user.cc`, unused terminal
objects) is free of unresolved Windows symbols. Members become reachable as M1+
uses them; a member that fails to link then is M1's finding, consistent with
the spike's representative-subset design. To close this gap deliberately rather
than let it surface late, M1's first gate is an **automated whole-archive/
force-link diagnostic** (e.g. `--whole-archive`, or an equivalent per-object
link audit) that enumerates every production archive — including
`libmoshnetwork.a` once the M1 WinSock port lands — and every member, using the
production flags and backend, and reports explicit pass/fail. Each member —
e.g. `statesync/user.cc` — is thereby checked for unresolved Windows symbols up
front, and the link map is preserved as evidence. The link map alone cannot
catch a `.cc` that was omitted from an archive's source list (an absent member
leaves no trace), so the gate must also treat the production build manifest as
the single source of truth and compare the intended source inventory against
the archive members before force-linking. Merely linking the first complete
executable is insufficient: members it does not reference stay unlinked and
their Windows-symbol gaps stay hidden. This must be an enforced target, not a
prose promise skipped when the first integrated executable happens to link.

What M0 does **not** establish, deferred to M4 (packaging): that the exe runs
from a clean environment with the toolchain directories removed from `PATH`,
that its full non-system DLL closure (including the transitive dependencies of
`libncursesw6`/`libcrypto`) is resolved and shipped, and that the staged bundle
is self-sufficient. The import check below is a **deny-list**, not a positive
allowlist: it proves the absence of named C++/protobuf runtime DLLs, not that
every remaining import is on an approved list. M4 owns the bundle contract, and
that contract must cover DLL-loading *security*, not just file presence: a
trusted install location with appropriate ACLs, safe Windows DLL-search
behavior (so a writable working directory cannot substitute a bundled
`libcrypto`/terminal DLL), code signing, a bundled-OpenSSL update policy, and
SBOM/licensing/provenance for every shipped DLL.

### M4 resolution (observed on CI)

M4 is complete and CI-green in run `31127885569`. `win32/package.sh` stages the
bundle and `win32/verify-bundle.ps1` verifies it in a scrubbed environment on
`windows-11-arm` with MSYS2 CLANGARM64. The exact observed transitive
non-system DLL closure is `libcrypto-3-arm64.dll` (OpenSSL), `libncursesw6.dll`
(ncurses), and `zlib1.dll` (zlib); zlib is bundled. The bundle also contains
`mosh.exe`, license notices, and `MANIFEST.txt` with per-file SHA-256, size, and
source, classified imports, and tool versions. Protobuf, Abseil, the LLVM C++
runtime (libc++/libunwind/compiler-rt), OCB, and winpthreads are folded into
`mosh.exe`, not shipped as DLLs. Under fail-closed classification every
`mosh.exe` import is either a Windows-system/UCRT import (classified `SYSTEM`)
or one of the three bundled DLLs above (classified `SHIPPED`); no import is
left unclassified.

The clean-environment probe establishes that `mosh.exe` reaches argument parsing
with MSYS2 off `PATH` and `TERM`/`TERMINFO` absent (exit 2), and that
`test_core.exe` runs to exit 0 from the bundle alone. A bundle copy with one DLL
removed fails to launch with `0xC0000135` (`STATUS_DLL_NOT_FOUND`), providing a
negative control against a vacuous self-sufficiency result. Thus M4 now
establishes file presence, the complete non-system DLL closure, and bundle
self-sufficiency. It does not establish the still-open security follow-ups:
trusted install location and ACLs, safe DLL-search behavior, code signing, a
bundled-OpenSSL update policy, or a complete SBOM/licensing/provenance contract;
the manifest and license notices do not close those items. The bundle contains
no terminfo: the client constructs `Display(false)`, while `mosh-server` sets
the remote `TERM` from `-c`, so the original bundle-terminfo plan item is
obsolete.

The import check is done via the portable `objdump -p` import table, not via
`nm -u` (which cannot see PE import-table entries) and not by CRT-name matching.

The check target is parameterized via `NM` and `OBJDUMP` variables and
CRT-agnostic. It verifies:
1. The built `mosh.exe` file exists.
2. No unresolved C++ runtime symbols (via `$(NM) -u`).
3. No dynamic imports of C++-runtime or protobuf DLLs (via `$(OBJDUMP) -p`),
   matched against a deny-list that includes `libstdc++`, `libc++`, `libgcc`,
   `libunwind`, `libwinpthread`, `libssp`, `libatomic`, protobuf/abseil, and the
   Abseil `utf8` helpers (`utf8_range`/`utf8_validity`).
4. No dependency on the MSYS/Cygwin POSIX-emulation runtime (`msys-2.0.dll`,
   `cygwin1.dll`). This enforces the native-MinGW target boundary and matters
   specifically because `check` runs inside the MSYS2 environment, where such a
   dependency would otherwise resolve silently and pass unnoticed.

The makefile `check` is intentionally CRT-agnostic because it serves both the
msvcrt local smoke build and the UCRT CI build. The **target UCRT ABI boundary**
is therefore enforced by a **CI-only step** (`Verify UCRT ABI`) that rejects a
direct `msvcrt.dll` import and requires the UCRT `api-ms-win-crt-*` API-set
imports — catching a build that silently linked the wrong CRT, which every
CRT-agnostic check would otherwise pass. This proves the executable's **direct**
CRT boundary only; it does not establish that dynamically-loaded `libcrypto`,
ncurses, or their transitive DLLs use the intended CRT or architecture — that
recursive DLL-graph inventory (machine type + CRT family + resolved path/hash
per non-system DLL) is an M4 item.

### Import sets (observed)

Two toolchains produce `mosh.exe`; their import sets differ because the local
image links OpenSSL statically while the CLANGARM64 package may link it
dynamically. The static-fold requirement (no C++-runtime/protobuf DLL) is
**confirmed to hold on both**: observed for the local image, and confirmed on
the CLANGARM64 target by the authoritative CI run recorded below.

**Local cross-build image** (`aarch64-w64-mingw32-objdump -p`, re-verified
2026-08-06 against the clang 22.1.8 ubuntu-base image):

- `msvcrt.dll`, `KERNEL32.dll`, `USER32.dll`, `ADVAPI32.dll`, `WS2_32.dll`,
  `bcrypt.dll`
- `CRYPT32.dll` — OpenSSL's certificate-store dependency; because this image
  links libcrypto statically, `crypt32` surfaces as a direct import of the exe
- `bcrypt.dll` — the WinSock port's CSPRNG (`-lbcrypt`); added by the M2
  socket-layer work, after this import set was first recorded
- absent: no C++-runtime DLL (`-static-libstdc++ -static-libgcc` folded
  libc++/libunwind; `nm -u` shows no `__cxa`/`_Z`/`_Unwind` residue), no
  libcrypto/tinfo/zlib DLL (all static in this image)

**CLANGARM64 CI runner** (`windows-11-arm` = Azure Cobalt 100 / Neoverse N2,
`llvm-objdump -p`): the authoritative crypto-linked import set, recorded from the
CI run on this commit (evidence below), is:

- UCRT API-set DLLs: `api-ms-win-crt-{stdio,runtime,locale,heap,private,string,
  convert,environment,math,time,multibyte,filesystem,utility}-l1-1-0.dll`
- Windows system: `KERNEL32.dll`, `ADVAPI32.dll`, `dbghelp.dll`
- bundled C libraries (dynamic, ship beside the exe): `libncursesw6.dll` and
  `libcrypto-3-arm64.dll`

This resolves the earlier static/dynamic-OpenSSL question: the CLANGARM64
package links **OpenSSL dynamically** (`libcrypto-3-arm64.dll`), so — unlike the
static-libcrypto local image — there is **no direct `crypt32.dll` import** (that
was a local-static artifact). Absent, as required: no `msvcrt.dll` (UCRT
confirmed), no C++-runtime DLL (`libc++`/`libunwind`/`libgcc`/`libstdc++`/
`libwinpthread`), no protobuf/Abseil/`utf8` DLL (static-fold confirmed with
protobuf 35.0 + abseil 20260526.0), and no MSYS/Cygwin runtime.

**Authoritative CI evidence** (fills the former placeholder; this is an
evidence-only documentation record, exempt from re-gating per the build-input
closure rule in Result):

| Field | Value |
| --- | --- |
| Commit (build inputs) | `ba642de0424217f2a517d9417949d8ff8bfd0917` |
| CI run | `andrewachen/mosh` Actions run `29928638939` (job `spike`, 2m49s, success) |
| Effective flags | `ARM_MCPU=-march=armv8-a -mtune=oryon-1` (baseline; product `-mcpu=oryon-1` validated at M5) |
| PE machine type | `coff-arm64` / `architecture: aarch64` (native ARM64, not emulated) |
| Undefined symbols | 0 (no C++-runtime undefs) |
| Runtime | `./mosh.exe` exit status `0` on native ARM64 (Cobalt N2) |
| `mosh.exe` SHA-256 | `19f9dba59ec6040214a9fef6f69fb10a4b87aadd29215f14b1e11331d58cb404` |
| Toolchain | clang 22.1.7 (`aarch64-w64-windows-gnu`), openssl 3.6.3, protobuf 35.0, abseil 20260526.0, ncurses 6.6, zlib 1.3.2 |

The deny-list requirement — no `libc++`, `libunwind`, `libgcc_s`, `libstdc++`,
`libwinpthread`, `libprotobuf`, or Abseil DLL — is an **acceptance requirement**
the gate enforces on every build; it is confirmed *observed* both for the local
image and, for the CLANGARM64 target, by authoritative CI run `29928638939`
on parent commit `ba642de` (import set recorded above). On MSYS2 clang the
`-static-libstdc++ -static-libgcc` flags fold the
LLVM C++ runtime
(libc++ / compiler-rt / libunwind) into the executable; a plain `-static` is
deliberately *not* used because it would also statically absorb the C libraries
that must stay dynamic and bundled.

### Runtime gate

At the historical M0 commit, CI executed the standalone spike `mosh.exe` after
building it, asserting a clean (0) exit and first asserting via `llvm-objdump
-f` that the PE was a native ARM64 image (`coff-arm64` / `aarch64`) rather than
an x64 binary running under Windows-on-ARM emulation. The executed binary was
the **baseline `-march=armv8-a` CI build** (not the `-mcpu=oryon-1` product
build), which made running it on the Cobalt N2 runner sound — see the CI-build
note above. That historical spike run exercised its AES-OCB round-trip and
terminal/statesync construction at runtime on genuine ARM64.

Today, CI's bare `./mosh.exe` invocation is deliberately the console frontend's
usage gate: it exits `2` before opening a console or constructing `MoshCore`.
It is neither a console/client runtime test nor crypto evidence. The historical
M0 runtime result was a **linkage/liveness gate**, not wire-compatibility or
authentication conformance evidence. Crypto conformance splits across two gates
because the CI runner (baseline N2) and the product build (Oryon) differ:

- **M1 entry gate (baseline, CI):** before any networking integration or
  production credentials, crypto conformance MUST run on the native CLANGARM64
  runner against the **baseline CI build** with the production-selected backend.
  A single known-answer vector plus one tamper case is insufficient for an
  authenticated wire protocol; this gate MUST run the full upstream mosh crypto
  test suite, plus standardized OCB known-answer vectors (e.g. the RFC 7253 OCB
  vectors at mosh's 128-bit key / 96-bit nonce / 128-bit tag parameters)
  covering empty, partial-block, block-boundary, and multi-block messages, plus
  negative cases for a modified nonce, ciphertext, tag, and truncated input, and
  MUST demonstrate **bidirectional** wire interoperability against a pinned,
  known-good mosh implementation (Windows-produced ciphertext decrypts there,
  and its ciphertext decrypts on Windows), with the fixtures/harness stored
  rather than pulled from an unspecified external install.
- **M5 release gate (Oryon, hardware):** the same full conformance suite MUST be
  re-run on the **exact `-mcpu=oryon-1` product binary** on Snapdragon X
  hardware before release eligibility. **Accepted residual risk:** because the
  M1 gate exercises the baseline codegen, a crypto defect specific to Oryon code
  generation would not be caught until M5. This risk is the deliberate cost of
  the Oryon-product / baseline-CI split; if it is unacceptable, an Oryon runner
  must be introduced at M1 to run this suite on the exact product binary
  (carrying that binary's digest unchanged through packaging to M5). The local
cross-build image cannot run the ARM64 binary (it produces Windows PE
executables, which qemu-aarch64 cannot execute — CPU emulation is not a
Windows userspace), so the runtime gate is CI-only; the local build stops at
the link and import checks.

### Crypto backend selection

`win32/Makefile.win` compiles `src/crypto/ocb_internal.cc`: mosh's own OCB
mode construction, whose AES-128 block operations run through OpenSSL's EVP
interface (`EVP_CIPHER_CTX` with `EVP_aes_128_ecb()`) under `USE_OPENSSL_AES`.
The low-level `AES_*` primitives this file once used were replaced by EVP in
upstream commit `1416e9a`, so both engine backends are EVP-based today; the
distinction is *where* OCB comes from. Upstream ships a second backend in
`src/crypto/ocb_openssl.cc` that delegates the entire OCB mode to OpenSSL
(`EVP_aes_128_ocb()`). The two define the same `ae_*` interface, only one may
be linked, and both are wire-compatible OCB-AES128.

This choice **matches the upstream default**. `configure.ac` defaults
`--with-crypto-library` to `openssl`, whose case sets
`AM_CONDITIONAL(USE_AES_OCB_FROM_OPENSSL, false)` and describes itself as
"internal OCB, OpenSSL AES" — i.e. `ocb_internal.cc` (internal OCB over OpenSSL
EVP-ECB), exactly what the spike links. The OpenSSL EVP-OCB backend
(`ocb_openssl.cc`) is selected only by the explicit non-default
`--with-crypto-library=openssl-with-openssl-ocb`. So no M1 re-decision is needed
to match upstream; a future switch to the OpenSSL-provided EVP-OCB backend would
be an intentional divergence to document then. No crypto source was modified
either way.

`libmoshnetwork.a` is intentionally not attempted. Its POSIX socket and
networking implementation needs the M1 WinSock port.

## Toolchain and dependencies

The M0a Docker image provides:

| Component | Observed version / location |
| --- | --- |
| C++ compiler | `aarch64-w64-mingw32-clang++`, clang 22.1.8 (llvm-mingw 20260616), target `aarch64-w64-windows-gnu` |
| Protocol compiler | `protoc 3.21.12` (`/opt/protobuf-host/bin`) |
| Protocol runtime | protobuf 3.21.12 (`/opt/mosh-arm64/lib/libprotobuf.a`) |
| OpenSSL target headers and library | 3.0.16 |
| zlib | 1.3.1 |
| terminal database | `/opt/mosh-arm64/lib/libtinfo.a` |

**ABI caveat:** the local image is an llvm-mingw `aarch64-w64-mingw32`
toolchain (clang + libc++/libunwind) whose CRT is the legacy `msvcrt.dll` —
*not* the target's UCRT. Its import set therefore lists `msvcrt.dll`
where the CLANGARM64 target lists the UCRT `api-ms-win-crt-*` API-set DLLs. Treat
the local image as an ABI-divergent **compile/link smoke test** that gives fast
feedback on source portability and the static-fold; it is not target-runtime
evidence. The CLANGARM64 CI job is the authoritative target check for the UCRT
ABI, build, link, imports, and baseline liveness (Oryon-ISA correctness remains
M5). The resolved
CLANGARM64 package/compiler versions are captured per-run by the workflow's
"Record toolchain and dependency versions" step (MSYS2 packages are rolling, so
a later run may use a materially different OpenSSL link mode or protobuf/abseil
closure). This is an **auditable snapshot, not a reproducible lock**: it records
the top-level package versions a given run validated, but does not capture the
full transitive closure (independently-versioned Abseil, utf8, compiler runtime)
and does not guarantee those exact package builds are re-obtainable later. The
per-run **evidence bundle** additionally captures the *full* `pacman -Q` closure
(not just the top-level packages) to files, so the exact Abseil/utf8/compiler-
runtime builds a run tested are recorded. Two durability limits remain: the
bundle is retained only for the repository's artifact-retention window, and its
`sha256` sits beside the binary rather than being an independent signed
attestation. A reproducible/release build would additionally pin a
package-repository snapshot (or a versioned build image), store the bundle in a
retention-independent location, and emit an SBOM + a provenance attestation
signed outside the artifact — deferred to release engineering / M4.

The current MSYS2 CLANGARM64 repository contains the CI package names
`mingw-w64-clang-aarch64-{openssl,protobuf,ncurses,zlib}`. The CI workflow
installs exactly those packages plus `mingw-w64-clang-aarch64-clang`,
`mingw-w64-clang-aarch64-pkgconf`, `make`, and `zip`.

The direct build uses:

```text
-I/opt/mosh-arm64/include -L/opt/mosh-arm64/lib
-lcrypto -lprotobuf -lz -ltinfo -lws2_32 -luser32 -lcrypt32 -DNCURSES_STATIC
```

`-lcrypt32` is required because the AES-OCB round-trip pulls OpenSSL's cipher
path, and OpenSSL's Windows build references the `crypt32.dll` certificate-store
API (`CertOpenStore` and friends). The local image links OpenSSL statically, so
those references resolve at the `mosh.exe` link and must be satisfied explicitly.

`protobuf.pc` advertises only `-lprotobuf`; `-lz` is therefore explicit. It is
also required by the mosh compressor code. The host `protoc` and target runtime
both report 3.21.12.

On CI with protobuf v22+ (which uses Abseil), `pkg-config --libs --static
protobuf` expands to include the Abseil and utf8-cpp closure. The local
image's protobuf 3.21.12 has no such closure.

`ARM_MCPU` override matrix. `win32/Makefile.win` defaults to
`ARM_MCPU ?= -mcpu=oryon-1` (the Oryon product target). The local image's
clang 22 accepts that flag, and local invocations use the default — the smoke
test exercises the same oryon-1 product codegen that M5 validates on
Snapdragon X hardware (verified: the built exe contains LSE `ldadd`/`swpal`
atomics that baseline aarch64 would not emit). CI passes
`ARM_MCPU="-march=armv8-a -mtune=oryon-1"` so the gate binary runs on the Cobalt
N2 runner (see the CI-build note at the top). As stated there, `-mcpu=oryon-1`
establishes an Oryon-class hardware target; it is *not* a portable
Windows-ARM64 default, is validated only at M5 on Snapdragon X hardware, and
must not silently ship as a general-release build.

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

From the mosh repository (the harness `Dockerfile.mosh-arm64` +
`build-mosh-arm64-local.sh` lives here), run:

```sh
MOSH_LOCAL=~/git/gh/mosh/.claude/worktrees/mosh-termination \
  ./build-mosh-arm64-local.sh
```

The `MOSH_LOCAL` override is mandatory when validating a worktree. Without it,
the script defaults to the canonical mosh checkout (`~/git/gh/mosh/`) and
silently validates the unchanged main-branch sources instead of the worktree.

The script runs the M0a dependency smoke test first, then invokes:

```sh
make -f win32/Makefile.win clean
make -f win32/Makefile.win CXX=aarch64-w64-mingw32-clang++ AR=aarch64-w64-mingw32-ar NM=aarch64-w64-mingw32-nm OBJDUMP=aarch64-w64-mingw32-objdump ARM_MCPU= check
```

inside the mounted mosh checkout. The check verifies the five archives, the
executable, its absence of unresolved C++ runtime symbols, and absence of
dynamic imports of C++/protobuf runtime DLLs (via the `objdump -p` deny-list).

For the MSYS2 CLANGARM64 CI build (`.github/workflows/clangarm64-spike.yml`),
the same makefile runs with the default `NM=llvm-nm` and `OBJDUMP=llvm-objdump`
and the baseline override `ARM_MCPU="-march=armv8-a -mtune=oryon-1"`. Beyond the
makefile `check`, CI adds the native-ARM64-PE assertion, the UCRT ABI check
(reject `msvcrt.dll`, require `api-ms-win-crt-*`), a bare frontend invocation
that asserts usage exit code `2`, and two artifact uploads: the durable
evidence bundle and the consumer-facing zips. `mosh-windows-arm64.zip` is the
CI-baseline binary (armv8-a, verified on the runner); `mosh-windows-arm64-oryon.zip`
is the `-mcpu=oryon-1` product binary, compile-only in CI because the Cobalt N2
runner cannot execute Oryon-specific codegen (e.g. SM4) — M5 validates it on
Snapdragon X hardware. Download either with
`gh run download <run-id> -n mosh-windows-arm64[-oryon]`. The
bare invocation occurs before console setup and `MoshCore` construction, so it
is not a console, client-core, or crypto runtime gate. The historical
crypto-linked spike build is validated by authoritative CI run `29928638939` on
parent commit `ba642de`, recorded in the import-set section above. This
evidence-only commit changes only that evidence record and therefore shares
`ba642de`'s build inputs (see the build-input-digest closure rule in Result).

M2 console/session runtime validation requires a real console and session on
the native Windows ARM64 runner or hardware; local Docker validation is
compile/link-only.

## Deferred to M1/M2

The following runtime findings were identified during the M0 spike and are
deferred to Milestone 1/2. They split into two classes: **production release
blockers** — the PRNG, timestamp monotonicity, and elimination of the spike-only
shims — which MUST pass before any production/release target is built, and
**compatibility/fidelity** items — wcwidth and locale behavior — which can land
independently. A build-time mechanism (see Spike-only shims) must prevent the
spike configuration and compatibility shims from silently entering a production
build.

- **PRNG** (release blocker): `/dev/urandom`/`getrandom`/`getentropy` are unavailable on Windows;
  key generation will throw at runtime until a Windows CSPRNG is wired in. Use
  `BCryptGenRandom` with `BCRYPT_USE_SYSTEM_PREFERRED_RNG` (the modern CNG API);
  the legacy `CryptGenRandom` is deprecated and should not be used for new code.
  Required invariant: a `BCryptGenRandom` failure MUST **fail closed** (abort key
  generation) with no fallback entropy source — never degrade to a weaker RNG.

- **wcwidth combining marks**: The BMP-only fallback in `src/terminal/terminal.cc`
  returns 1 for combining marks; it should return 0.

- **Locale API**: `src/util/locale_utils.cc` should use `GetACP()` instead of
  `LOCALE_IDEFAULTANSICODEPAGE`, and locale-variable clearing should use
  `_putenv_s` to sync the CRT environment.

- **Timestamp monotonicity** (release blocker): The `gettimeofday` fallback selected in
  `src/util/timestamp.cc:113` is non-monotonic. M1 must choose between MinGW
  `clock_gettime(CLOCK_MONOTONIC)` with runtime probing, or a
  `QueryPerformanceCounter` / `GetTickCount64` shim.

- **Spike-only shims** (release blocker): The no-op signal handlers,
  non-monotonic clock, and no-op core-dump shims in `win32/posix_compat.h`,
  `src/util/timestamp.cc`, and `src/crypto/crypto.cc` MUST be gated behind an
  explicit build-time mechanism (e.g. a spike-only define that a production
  target refuses) so they cannot leak into a production build. The build-time
  refusal is necessary but not sufficient: the production build must also define
  the *required Windows behavior* the shims stand in for — in particular the
  Windows counterpart of `disable_dumping_core()`, i.e. preventing Windows Error
  Reporting / crash dumps from persisting session keys or other secrets — not
  merely remove the no-op.

## M2 Steps 6-8: Windows console frontend

`mosh.exe` now uses `win32/mosh_main.cc`, `win32/console_io.cc`, and the
OS-agnostic `MoshCore` rather than the build spike entry point. The console
frontend captures and restores its input/output modes and code pages, enables
VT input/output with UTF-8 code pages, and owns the alternate-screen lifecycle
with a scope guard so the terminal is restored before diagnostics are printed.

The event loop has a dedicated blocking `ReadFile` reader thread. It queues
conhost's VT bytes behind an auto-reset event, converts CESU-8 surrogate pairs
to UTF-8 across read boundaries, polls screen dimensions at most 100 ms apart,
and reconciles `WSAEVENT` registrations after every mutating core tick. Reader
and socket-event RAII owners cancel/join and release resources on normal and
exceptional exits. `mosh.exe` takes one `<user@host>` destination; every other
argument shape prints usage and returns exit code 2.

The makefile links `mosh_main.o`, `console_io.o`, and `mosh_core.o` into
`mosh.exe`, and compiles `console_io_include_check.cc` through both `all` and
`check`. That translation unit includes `console_io.h` before every other
project header, preserving the `winsock2.h`-before-`windows.h` constraint.
The CI bare invocation asserts usage exit code `2`; it does not exercise the
console frontend or `MoshCore`. The local Docker gate is compile/link-only, and
real console/session validation remains native Windows ARM64 work.

Local verification command (from the mosh repository):

```sh
./build-mosh-arm64-local.sh
```

The command completed successfully on this revision (exit 0, with no
`error:` or `fatal error:` diagnostics).
