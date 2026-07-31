# MoshCore / STMClient behavioral-parity inventory

`win32/mosh_core.cc` preserves the shared transport, overlay, prediction, and framebuffer algorithms, but it is not a literal extraction of `STMClient`: the host loop, console lifecycle, startup configuration, and several lifecycle and error paths differ. This inventory records those differences so each one is either justified or scheduled, rather than discovered later as a bug.

Findings name the symbol they concern and give a line number as a search hint only. Line numbers drift; symbol names do not. Treat a line number that does not match as a stale hint, not as evidence the finding is obsolete.

## Classification

| Class | Meaning |
|---|---|
| `PLATFORM` | Windows genuinely cannot provide upstream's mechanism. Not correctable. |
| `POLICY` | A deliberate product decision. Correctable, but we do not want to. |
| `DEFERRED` | A parity defect with corrective work already scheduled. |
| `DEFECT` | Unintended drift. Should be corrected; not yet scheduled. |
| `OPEN` | Not yet resolved either way. |

Severity is **high** for correctness, security, or resource-exhaustion consequences; **medium** for user-visible behavior; **low** for diagnostics and cosmetics.

## Count table

| Class | Findings |
|---|---:|
| `PLATFORM` | 7 |
| `POLICY` | 2 |
| `DEFERRED` | 0 |
| `DEFECT` | 22 |
| `OPEN` | 2 |
| **Total** | **33** |

Eight repaired or confirmed behavior records — connection-timeout shutdown, reader input ending, interrupt control events versus a typed Ctrl-C, bounded close-handler restoration attempts, post-wait timestamp freezing, resize framebuffer ownership, UCRT wide-printf semantics, and the retired command-line key channel — are recorded at the end and are not counted as findings.

## How the defects cluster

Seven are **configuration omissions**, each independent and individually cheap: `MOSH_ESCAPE_KEY` (A1), `MOSH_PREDICTION_OVERWRITE` (A2), the prediction display preference (A25), the `[mosh] ` title prefix (A3), `-v` diagnostics (A4), `MOSH_NO_TERM_INIT` (A20), and the escape-suspend sequence (A16).

Eleven are **event-loop and shutdown drift**: A5, A6, A7, A8, A9, A12, A15, A18, A22, A23, and A24. `STMClient::main()` was re-derived rather than extracted, so every ordering and exception-boundary decision was re-made independently, and each drifted on its own.

Whether the correct remedy is a shared platform-neutral loop coordinator (taking normalized events, returning actions and deadlines) or platform-specific loops held together by parity tests is an open architectural question — a literal extraction of `STMClient::main()` is unlikely to stay simple, because the POSIX and Win32 waiting, input, resize, and termination contracts genuinely differ.

What must be settled first is the **behavioral contract**, not the code organization: the intended phase ordering, exception boundaries, timer semantics, fairness limits, and shutdown invariants, recorded as expected event traces rather than prose — ordinary input, simultaneous input and network readiness, receive error, send error, crypto error, resize during shutdown, a typed Ctrl-C reaching the remote, a first and a repeated interrupt control event, and close or session-end termination. Those traces serve either architecture and make the eventual coordinator decision evidence-based. Sharing an implementation stays an evaluated option, not a prerequisite.

Only the genuinely coupled findings wait on that contract: A5 and A9 (phase ordering) and A6, A7, and A15 (exception boundaries and retry timing). A18 is an independent safety invariant with a local contract test and should land now — it is the only high-severity defect in this cluster, and holding a resource-exhaustion fix behind a speculative refactor is the wrong trade. It is not the only high-severity finding in the inventory: S2 is the other, and it is a release blocker. A22 is local error-state bookkeeping and is likewise independently fixable.

The remaining defects are exit-path omissions (A13, A14, A21) and one security defect (S2).

### The host loop, for reference

Several findings below depend on the exact phase order, so it is stated once here. `ConsoleSession::Impl::run_loop()` (`win32/console_io.cc:1093`) runs:

```
tick  →  wait (timeout capped at RESIZE_POLL_CAP_MS = 100 ms)  →  refresh clock
      →  termination check  →  reader-failure check  →  resize poll
      →  at most one readable socket  →  input  →  frame  →  (next iteration) tick
```

There is no second wait between dispatch and the next `tick()`. Upstream's order is frame → wait → dispatch → tick, all in one pass.

---

## Findings

### Startup, configuration, and terminal model

#### I1. Native-console terminal lifecycle replaces termios and terminfo

* **Upstream:** `STMClient::init()` saves and raws `stdin` with termios and opens the terminal through the terminfo-configured `Display` (`src/frontend/stmclient.cc:82`); `Display(true)` consults terminfo including `smcup`/`rmcup` (`src/terminal/terminaldisplayinit.cc:83`).
* **Port:** Constructs `Display(false)` and delegates raw mode, UTF-8 code pages, and VT output to `ConsoleSession` (`win32/mosh_core.cc:112`, `win32/console_io.cc:841`).
* **Consequence:** The executable uses the Windows Console VT contract rather than the user's terminfo.
* **Class:** `PLATFORM`, low. Windows has no termios, and the native console requires `SetConsoleMode` plus VT escapes.

#### A20. `MOSH_NO_TERM_INIT` is ignored and the alternate screen is unconditional

* **Upstream:** `Display::open()`/`close()` emit `smcup`/`rmcup` only when terminfo initialization was not suppressed; `MOSH_NO_TERM_INIT` suppresses it (`src/terminal/terminaldisplayinit.cc:83`).
* **Port:** Wraps `display.open()`/`close()` in literal `\033[?1049h`/`\033[?1049l` unconditionally (`MoshCore::Impl::Impl`, `win32/mosh_core.cc:154`).
* **Consequence:** A user who sets `MOSH_NO_TERM_INIT` to keep the session on the primary screen buffer gets the alternate screen anyway, and scrollback is hidden for the session.
* **Class:** `DEFECT`, low. This is a separate decision from I1 — nothing about the Windows console forces the alternate screen. Splitting it out matters because I1 is not correctable and this is.
* **Fix:** Gate the two literal sequences on the same environment variable.

#### A1. `MOSH_ESCAPE_KEY` configuration is omitted

* **Upstream:** Parses `MOSH_ESCAPE_KEY`, accepts one ASCII key or disables the parser on an empty value, derives the literal-pass spelling and line-start rule, and rejects dangerous controls (`src/frontend/stmclient.cc:130`).
* **Port:** Hardcodes Ctrl-^ / `^` / no line-start requirement and builds only that help string (`win32/mosh_core.cc:114`, `win32/mosh_core.cc:135`); there is no environment lookup.
* **Consequence:** A Windows user cannot select another escape prefix or disable the escape parser. Bytes upstream would pass through as literal data instead invoke the local command parser.
* **Class:** `DEFECT`, medium. Pure parser configuration; no console constraint applies.

#### A2. `MOSH_PREDICTION_OVERWRITE=yes` is omitted

* **Upstream:** Enables insertion-overwrite prediction when the variable is exactly `yes` (`src/frontend/stmclient.h:119`), read at `src/frontend/mosh-client.cc:178`.
* **Port:** Accepts only a display-preference argument and never calls `PredictionEngine::set_predict_overwrite()` (`win32/mosh_core.cc:121`, `win32/mosh_main.cc:165`).
* **Consequence:** Insert and delete prediction can visibly differ from upstream before the server echo arrives.
* **Class:** `DEFECT`, low. The shared prediction engine already exposes the operation.

#### A25. The prediction display preference has no source

* **Upstream:** `mosh-client` reads `MOSH_PREDICTION_DISPLAY` from the environment, where an absent variable is allowed (`src/frontend/mosh-client.cc:174`); the wrapper sets it from `--predict` (`scripts/mosh.pl:463`).
* **Port:** `MoshCore` maps all four upstream preference values and throws on an unrecognized one (`win32/mosh_core.cc:121`), but its only caller passes the literal `"adaptive"` (`win32/mosh_main.cc:165`) and no environment variable is read.
* **Consequence:** A Windows user cannot select `always`, `never`, or `experimental`. On a high-latency link the underlined prediction display cannot be forced on, and on a local link it cannot be turned off.
* **Class:** `DEFECT`, low. The engine already implements every preference; only the source is missing.
* **Note:** upstream's environment variable is the natural source. Supplying one is a distinct change from retiring the command-line key channel and is deliberately not bundled with it, so the removal stays a removal.
* **Contract for the fix:** read and *validate* `MOSH_PREDICTION_DISPLAY` before calling `mosh_bootstrap`, then hand the already-validated preference to `MoshCore`. The accepted values are `always`, `never`, `adaptive`, and `experimental`. An absent variable is not an error and selects nothing: the engine's own default is `Adaptive` (`src/frontend/terminaloverlay.h:311`), which is what the sole call site passes literally today. Any other value, the empty string included, is refused rather than silently downgraded. The ordering is the substance of this contract, not a detail. `mosh_bootstrap` starts a detached `mosh-server` on the remote host before `MoshCore` is constructed (`win32/mosh_main.cc:161`, `win32/mosh_main.cc:165`), so leaning on `MoshCore`'s existing throw (`win32/mosh_core.cc:131`) would refuse the value only after stranding a remote server to sit out its no-client timeout. Upstream refuses first as well, in the wrapper, before it opens `ssh` (`scripts/mosh.pl:185`, `scripts/mosh.pl:143`). Validated locally, the remaining divergence is exit status alone, of the kind recorded under I7. The preference is a display choice with no bearing on the endpoint or the key, so reading it does not reopen the channel S1 closed.
* **Acceptance coverage for the fix:** an invalid value and an empty value each fail without spawning `ssh`; an absent variable leaves `Adaptive`; each of the four accepted values reaches the core.
* **The rule generalizes, and should be applied that way:** every locally detectable configuration error must be resolved before `mosh_bootstrap` starts a remote server. That covers the rest of the configuration family — `MOSH_ESCAPE_KEY` (A1), `MOSH_PREDICTION_OVERWRITE` (A2), `MOSH_NO_TERM_INIT` (A20) — along with terminal initialization and any dump protection S2 introduces, which upstream establishes before it acquires the key at all. Each should also take an owned, parsed snapshot at validation time rather than carrying a `getenv` pointer across process creation: a typed preference leaves `MoshCore` nothing to re-validate, so there is no second validation point to drift from the first.

#### A3. The `[mosh] ` title prefix is never set

* **Upstream:** `STMClient::init()` sets the prefix unless `MOSH_TITLE_NOPREFIX` is set (`src/frontend/stmclient.cc:125`); `STMClient::shutdown()` clears it before the final frame (`src/frontend/stmclient.cc:209`).
* **Port:** Never sets or clears an overlay title prefix (`win32/mosh_core.cc:111`).
* **Consequence:** Remote title updates are not marked as mosh titles during the session.
* **Class:** `DEFECT`, low. `Display(false)` still has title support (`src/terminal/terminaldisplayinit.cc:83`), so this is not forced by the display choice.
* **Note:** Upstream's shutdown *clears* the prefix; it never saves or restores whatever title existed before mosh started. Because the port never adds a prefix, both implementations leave the same bare remote title after exit. Only the in-session prefix differs.

#### A4. Upstream's `-v` diagnostics are unavailable

* **Upstream:** Parses repeatable `-v` (`src/frontend/mosh-client.cc:129`) and calls both `network->set_verbose()` and `Select::set_verbose()` (`src/frontend/stmclient.cc:260`).
* **Port:** Accepts only `<user@host>`, which takes no verbosity flag (`win32/mosh_bootstrap.cc:365`), and never sets transport verbosity (`win32/mosh_core.cc:150`).
* **Consequence:** Neither transport diagnostics nor poll diagnostics can be requested, making field troubleshooting harder.
* **Class:** `DEFECT`, low.
* **Fix:** Two tiers with very different costs. Transport verbosity is a flag plus one existing setter. Reproducing `Select`'s poll diagnostics has no shared setter to call and requires instrumenting the Win32 loop; see A18, which covers the behavior those diagnostics describe.

#### I7. The executable is a standalone CLI, not a wrapper-invoked client

* **Upstream:** `mosh-client` is wrapper-facing: it parses `-c`/`-v`, reads prediction settings from the environment, and returns `!success` (`src/frontend/mosh-client.cc:129`).
* **Port:** A standalone executable that spawns `ssh` for a `<user@host>` target and reads the endpoint from the server's `MOSH CONNECT` reply, returning distinct `2`, `3`, or `4` for usage, frontend, and exception errors (`win32/mosh_main.cc:46`).
* **Consequence:** Scripts written against `mosh-client` do not invoke `mosh.exe` compatibly, and error classes carry different numeric statuses. `mosh.exe` also collapses upstream's wrapper-plus-client pair into one process, so there is no separate client binary to invoke.
* **Class:** `POLICY`, low. `mosh.exe` is deliberately standalone and is not required to mirror `mosh-client`'s invocation or exit statuses.
* **Scope:** This finding covers invocation shape and exit status only. Credential transport is a separate question, recorded as S1 and now repaired — "standalone" does not imply "key on the command line."

#### I9. Hosts whose client-visible address the server cannot report are unreachable

* **Upstream:** the wrapper discovers a client-visible address before starting the session. Its default `proxy` method installs a `ProxyCommand` of its own, pointing back at the wrapper's `--fake-proxy` mode, and pairs it with `-S none` (`scripts/mosh.pl:402`); `local` instead resolves the target client-side. Neither is a free lunch — `man/mosh.1` warns that the proxy method can fail with bastion hosts and that `local` ignores matching `ssh_config` stanzas — so upstream does not simply honor the user's configured route either. What it does have is a mechanism; the port has none.
* **Port:** implements neither method, so the UDP target is always the server-reported `SSH_CONNECTION` address (`win32/mosh_bootstrap.cc:145`), and the bootstrap pins `ProxyJump` and `ProxyCommand` to none (`win32/mosh_bootstrap.cc:172`).
* **Consequence:** behind NAT or a load balancer, `ssh` connects normally, `mosh-server` starts, and the address it reports is the server's own private one — so the UDP session times out with no diagnostic naming the address it tried, and a remote server is left running. The retired positional endpoint form was the only override, and it is gone.
* **Class:** `POLICY`, medium. Out of scope by decision: the supported case is a host the client can route to directly, and no discovery work is scheduled. Recorded as a finding because it is a user-visible behavioral difference from upstream, not because it is planned. Revisit if a supported deployment lands behind NAT or a load balancer; nothing else should reopen it.
* **Fix, if the decision is revisited:** an override needs nothing secret on the command line — see the S1 record, which separates endpoint selection from key transport. Its shape is not settled here, and two values are involved that need not be equal: the port the server binds, which `mosh-server new -p PORT[:PORT2]` can already request (`src/frontend/mosh-server.cc:234`, forwarded by upstream as `--port`, `scripts/mosh.pl:159`) and this bootstrap does not expose, and the address and port the client aims at. An address rewrite alone covers only port-preserving translation. Implementing upstream's `local` or `proxy` discovery is the other path and keeps the interface unchanged.
* **Related:** A21 covers the missing exit diagnostics. This is where they would matter most: a timeout that named the address it tried would turn an unexplained hang into an answer. I10 covers the proxy pins, which are a separate restriction.

#### I10. Configured SSH routes are refused outright

* **Upstream:** honors the user's `ProxyJump` and `ProxyCommand` in its `remote` mode. Its other two discovery modes do not: `proxy` substitutes a `ProxyCommand` of its own (`scripts/mosh.pl:402`), and `local` rewrites the destination to a numeric address, which drops any `ssh_config` stanza matched on the host alias.
* **Port:** pins `ProxyJump=none` and `ProxyCommand=none` on every invocation (`win32/mosh_bootstrap.cc:172`), so a destination reachable only through a jump host fails at `ssh`.
* **Consequence:** sessions that would work are rejected. A bastion required for TCP/22 by policy does not imply an unroutable UDP address — the target's own address may be reachable over a VPN — but the bootstrap refuses the route before it learns the endpoint.
* **Class:** `OPEN`, medium. No rationale was recorded where the pins were introduced (`aa41fc8`), and the two this document previously offered do not hold: they do not stabilize the parsed stream, since those options select the transport beneath the SSH session rather than the remote's stdout channel, and they do not follow from I9's missing discovery, since a proxied route can announce a routable address. Either an invariant justifies failing closed and should be written down with its acceptance cases, or the pins should be lifted and the guarantee for a proxied route stated concretely — including whether an unroutable endpoint after a successful bootstrap is an accepted timeout, what it must print, and whether leaving the detached server to expire is accepted. Not `-S none`, which is a separate decision with a real reason; see S2.
* **Verification:** none. The command line is asserted exactly (`win32/test_bootstrap.cc:310`), which records the pins without testing what they exclude. Closing this means outcomes, not environments: a bastion-routed SSH with a directly routable UDP endpoint completes a session, and a jump-only private endpoint fails in bounded time naming the endpoint it tried. `ProxyJump` and `ProxyCommand` need separate cases — their precedence and behavior differ, and a bastion does not exercise an arbitrary proxy command.
#### I8. The bootstrap forces no remote PTY, where upstream requests one

* **Upstream:** the wrapper defaults `$ssh_pty` to 1 (`scripts/mosh.pl:84`) and so passes `-tt` (`scripts/mosh.pl:359`), merging the server's stderr into the stream it parses (`scripts/mosh.pl:356`). `--no-ssh-pty` turns that off (`scripts/mosh.pl:167`).
* **Port:** passes `-T` unconditionally alongside `-n` (`win32/mosh_bootstrap.cc:164`), with no option to request a terminal. `-n` alone would not settle it: it redirects only the client's stdin, so a `RequestTTY=force` in the user's configuration would still allocate one.
* **Consequence:** `mosh-server` 1.2.4 and older exit when their initial `TIOCGWINSZ` fails and cannot be reached from this client; 1.2.5 falls back to 80x24 and works. That exclusion is not created by `-T`, and reading it that way overstates the option's cost: `ssh` allocates no terminal for a remote command unless one is requested, so the port never had a PTY to begin with. What `-T` removes is the single configuration that could reintroduce one, `RequestTTY force`, and with it a user's ability to reach a pre-1.2.5 server by that accident. `RequestTTY yes` is not in that set: it requests a terminal only when this side's standard input is one, and `-n` makes standard input the null device.
* **Class:** `OPEN`, medium — servers this client cannot reach at all is user-visible behavior, not a diagnostic difference, and the choice is genuinely unresolved rather than deliberately made. `POLICY` would claim the alternative is understood and unwanted; the rest of this entry says otherwise. The bootstrap parses a startup banner out of `ssh`'s piped stdout, and one stream shape it can rely on is a requirement of that design: the parse must not depend on settings the client does not control. Be exact about what is and is not claimed, though — no parse failure under a PTY has been reproduced. Upstream merges stderr into the parsed stream deliberately, this parser ignores lines that are not `MOSH ` records and strips a trailing carriage return, so a terminal would plausibly work. The case for `-T` is that a configuration nobody tests should not be able to change what the parser sees, and its cost is bounded to users who force a terminal. Supporting both shapes instead would mean testing both against every server the matrix admits. One deterministic shape does not uniquely select `-T`, either: a forced `-tt` is equally deterministic, is upstream's default, and would keep pre-1.2.5 servers reachable. It is not chosen here because `-T` is what the port already did — `ssh` allocates no terminal for a remote command unless asked — so `-T` pins the current behavior while `-tt` would change it, in a direction where merged stderr and line discipline reach the parser and nothing has been tested. Settling that properly means comparing both end to end over banners, remote stderr, CRLF, and the oldest admitted server. Until that comparison exists, `-T` is an interim posture — the conservative pin on current behavior — not a decided policy, which is why this is `OPEN`. Closing it means either running the matrix and preferring `-tt` if it passes, or establishing `mosh-server` 1.2.5 as a supported minimum for a reason that stands on its own.
* **Verification:** the expected command line is asserted exactly (`win32/test_bootstrap.cc:310`). That proves the option is emitted, not that a hostile configuration cannot defeat it; a case with `RequestTTY force` in a controlled configuration, asserting the remote sees no terminal, belongs to M4's end-to-end matrix.

#### S2. Core-dump protection is a no-op and is never called

* **Upstream:** `mosh-client` calls `Crypto::disable_dumping_core()` as one of the first statements in `main()`, before it reads `MOSH_KEY` (`src/frontend/mosh-client.cc:112`); `mosh-server` does the same (`src/frontend/mosh-server.cc:181`). The implementation sets `RLIMIT_CORE` to zero and exits on failure (`src/crypto/crypto.cc:292`).
* **Port:** `win32/mosh_main.cc` never calls it, and the entire function body is inside `#ifndef _WIN32` (`src/crypto/crypto.cc:294`), so calling it would do nothing. The Windows build has no equivalent protection.
* **Consequence:** A crash after the key is in memory can persist it to disk through Windows Error Reporting or any other minidump path. S1's repair does not reach this: keeping the key out of `argv` keeps it out of the process parameter block but leaves it in the heap that a dump captures.
* **Class:** `DEFECT`, **high** — security. `BUILD-CLANGARM64.md` already records the missing Windows counterpart as a release blocker; this finding exists so the parity inventory does not imply the security surface is covered by S1 alone.
* **Fix:** define and implement the Windows behavior the no-op stands in for, rather than only gating the shim out of production builds. "Suppress crash dumps" is not by itself an implementable requirement: normal WER reporting, administrator-configured LocalDumps, application-installed crash handlers, attached debuggers, and external processes calling the dump APIs are distinct producers, and an unprivileged process cannot categorically stop all of them — just as `RLIMIT_CORE=0` does not stop a privileged debugger from reading process memory. Enumerate the producers and state for each whether it must be prevented, must fail startup, requires installer or system policy, or is outside the threat model. Scope parity to the automatic crash persistence the process itself controls, call the protection before the key is acquired, and fail closed when the promised protection cannot be established.
* **Verification:** a deliberately crashing helper under each supported WER and LocalDumps configuration, asserting no dump containing the key is written for the producers declared in scope.
* **Connection sharing is refused so that the child holding the key is the one this process launched.** `-S none` (`win32/mosh_bootstrap.cc:172`) is grouped with the proxy pins on the command line but is not the same decision, and unlike them it has a reason. With a `ControlMaster` configured, the session would be carried by a pre-existing master: that process owns the encrypted channel and therefore handles the plaintext `MOSH CONNECT` line and its key, it is long-lived, it is outside the bootstrap's Job Object so it is not reaped when the bootstrap ends, and it need not be the binary or version any packaged check validated. Every clause of the disposition below assumes the key-holding child is the one just launched. Keep `-S none` pinned unless that trust boundary is deliberately widened, which would mean specifying the master's provenance, version, dump exposure, key lifetime, and how the control endpoint is authenticated. This is why I10 covers only `ProxyJump` and `ProxyCommand`.
* **`ssh.exe` is a trusted external dependency, not a producer in scope.** The child holds the same key — it buffers the `MOSH CONNECT` line before writing it to the pipe — and it is whatever `SearchPathW` finds for `ssh` on the user's `PATH` (`win32/mosh_bootstrap.cc:234`). The port hardens the lookup as far as it can from inside the process: `lpPath` is passed explicitly, so the implicit current-directory and application-directory search is excluded (`win32/mosh_bootstrap.cc:218`), and an unset `PATH` fails closed rather than falling back to a default search. Beyond that, an OpenSSH client the user has installed and put ahead on `PATH` is trusted the same way the user's shell trusts it. This is not a divergence to inventory: upstream's wrapper invokes a bare `ssh` resolved from `PATH` (`scripts/mosh.pl:78`, `scripts/mosh.pl:409`) and additionally lets `--ssh=` replace the whole command (`scripts/mosh.pl:166`), so the port trusts strictly less than upstream does. Nor does it collapse into S1's threat model, once that model is stated in capabilities rather than identities: S1 answers an adversary that can *observe* — enumerate processes and read their command lines — while substituting `ssh.exe` requires control of what `ssh` *resolves to* — write access to the resolved binary, or to a directory the search reaches before it — which already owns the SSH session with or without mosh. Writing a `PATH` directory that the search never reaches first is not that capability. Both exclusions stand however the capability was obtained rather than only under full-token compromise, a delegated ACL on one early directory being enough; and reading another process's memory is excluded on the same terms, since it defeats the repair without touching a command line at all. The residual risk — a hostile or dump-configured `ssh.exe` capturing the key — is accepted, and constraining resolution to the system OpenSSH is deliberately not done. Anything that changes that judgment (shipping a bundled client, an installer that pins the path) reopens this bullet.
* **Related:** dump suppression and zeroization are complementary, not interchangeable. Suppressing one dump path does not remove residual copies left by uncontrolled `std::string` ownership, and end-of-session clearing does nothing for a mid-session crash. Only the *expanded* cipher context is inherently session-lived: `Session::Session` passes `key.data()` to `ae_init()` and every later encrypt and decrypt uses `ctx` (`src/crypto/crypto.cc:161`), so the encoded and decoded bootstrap copies can be zeroed immediately after initialization rather than held for the session. Specify which objects own the encoded key, the decoded key, and the expanded state, and when each is cleared.

### Event loop, timing, and network dispatch

#### I2. Win32 waits on handles rather than `pselect` fds and POSIX signals

* **Upstream:** Registers network fds and `STDIN_FILENO` and `pselect`s them, consuming SIGWINCH, SIGCONT, SIGTERM, SIGINT, SIGHUP, and SIGPIPE (`src/frontend/stmclient.cc:447`). `Select::select()` atomically unblocks signals for the wait (`src/util/select.h:143`).
* **Port:** Waits with `WaitForMultipleObjects` on the reader event, the termination event, and `WSAEventSelect(FD_READ)` events (`win32/console_io.cc:1137`); the console handler translates control events into the termination event (`console_control_handler`, `win32/console_io.cc:609`).
* **Consequence:** Control events and socket readiness are dispatched through handles rather than signals and fds.
* **Class:** `PLATFORM`, low.

#### I3. Reader thread, CESU-8 conversion, bounded queue, and fair dispatch replace `read(stdin)`

* **Upstream:** Reads up to 16 KiB synchronously from `STDIN_FILENO` once select reports it readable, then processes every returned byte in that pass (`STMClient::process_user_input`, `src/frontend/stmclient.cc:310`).
* **Port:** A worker blocks in `ReadFile`, recombines conhost CESU-8 surrogate pairs, and queues up to `INPUT_QUEUE_CAP_BYTES` (4 MiB); the owner consumes at most `INPUT_BUDGET_BYTES` (64 KiB) per wakeup (`Reader::read_loop`, `win32/console_io.cc:335`; `win32/console_io.cc:1218`).
* **Consequence:** A large paste is deliberately spread across loop iterations, so termination, resize, and socket work stay responsive; Windows input encoding is normalized before the shared parser sees it.
* **Class:** `PLATFORM`, low. Console `ReadFile` does not provide selectable byte-stream semantics, and an unbounded drain would starve the other handle sources.

#### I4. One readable socket is serviced per wakeup

* **Upstream:** Inspects every fd in the ready set but records only a boolean, invoking `process_network_input()` once per loop (`src/frontend/stmclient.cc:476`).
* **Port:** Re-tests the snapshot's WSA events, calls `core.on_readable()` for the first `FD_READ`, then breaks because the receive path can prune sockets (`win32/console_io.cc:1202`).
* **Consequence:** Both process one transport receive action per ordinary wakeup; the port additionally avoids dereferencing a snapshot invalidated by pruning.
* **Class:** `PLATFORM`, low.

#### A18. The zero-timeout poll throttle is absent

* **Upstream:** `Select::select()` counts consecutive zero-timeout calls and raises the timeout to 1 ms from the tenth onward, resetting the counter when a nonzero timeout appears (`src/util/select.h:131`, `MAX_POLLS = 10`).
* **Port:** Passes the computed timeout straight through `std::min( timeout, RESIZE_POLL_CAP_MS )` to `WaitForMultipleObjects`, so a zero stays zero indefinitely (`win32/console_io.cc:1130`).
* **Consequence:** When the transport or an overlay repeatedly returns a zero wait time, the port busy-spins at full CPU where upstream throttles to 1 ms. This is a live resource-exhaustion risk, not a diagnostic difference — upstream's `-v` messages merely *report* the throttle that the same code performs.
* **Class:** `DEFECT`, **high**.
* **Fix:** Replicate the consecutive-poll counter in `run_loop()`. Verification: drive the loop with a stub that returns zero repeatedly and assert the observed wait floor.

#### A5. `tick()` runs before the wait, so frame generation sits between dispatch and transmission

* **Upstream:** Processes ready network, input, and resize work and then calls `network->tick()` in the same pass (`src/frontend/stmclient.cc:486`).
* **Port:** Calls `core.tick()` at the top of the iteration, before the wait (`win32/console_io.cc:1099`); dispatch happens after the wait, and the next `tick()` is reached only after `core.next_frame()` has been generated and written (`win32/console_io.cc:1223`).
* **Consequence:** Bytes entered or state received in a wakeup are transmitted on the *next* iteration's tick rather than the current pass. Because no second wait intervenes, the added delay is the cost of frame generation and console output, not a poll interval — but that cost is unbounded under console output backpressure, and it is paid before every transmission.
* **Class:** `DEFECT`, medium.
* **Note:** The resize-poll cap does not add nearly 100 ms here: the loop returns directly to `tick()` after rendering.

#### A15. Up to 32 datagrams are drained per readable event, and a crypto error does not stop the drain

* **Upstream:** `process_network_input()` calls `network->recv()` exactly once (`src/frontend/stmclient.cc:296`). Its crypto catch is around the whole main loop, so the first nonfatal `CryptoException` abandons the rest of that pass.
* **Port:** Loops up to `MAX_DATAGRAMS_PER_READABLE` (32), breaking only on the synthetic `No packet received` condition or a network error (`MoshCore::Impl::process_network_input`, `win32/mosh_core.cc:197`). A nonfatal `CryptoException` executes `continue` (`win32/mosh_core.cc:212`), so the drain proceeds.
* **Consequence:** Burst traffic is applied sooner, but input, frame, and termination work can queue behind up to 32 decrypt-and-apply operations, and overlay acknowledgement updates are repeated per datagram. Because crypto failures do not break the loop, a burst of 32 malformed datagrams costs 32 decrypt attempts and 32 overlay notification writes in one wakeup, where upstream would attempt one.
* **Class:** `DEFECT`, medium.
* **Why the drain is unnecessary:** `FD_READ` is level-triggered. `recvfrom` is the call that re-enables it, and Microsoft documents that "if the reenabling routine is called and the relevant network condition is still valid after the call, the network event is recorded and the associated event object is set," concluding that "a single recv in response to each `FD_READ` network event is appropriate." Queued datagrams therefore re-signal the event without a new packet arriving, so one receive per wakeup cannot strand them.

#### A6. A receive-side `NetworkException` has no retry delay

* **Upstream:** Catches network exceptions around the entire loop, shows the error, sleeps 200 ms via `nanosleep`, and refreezes the clock (`src/frontend/stmclient.cc:562`).
* **Port:** Catches receive exceptions inside `process_network_input()`, reports and breaks the drain (`win32/mosh_core.cc:204`); `MoshCore::on_readable` then continues into lifecycle processing (`win32/mosh_core.cc:360`). Only a *tick*-side exception requests any delay.
* **Consequence:** Repeated receive failures are retried at the normal event cadence rather than rate-limited, increasing wakeups and repeated error handling.
* **Class:** `DEFECT`, medium. Introduced by moving exception boundaries.

#### A22. A tick-side network error is cleared before it can be rendered

* **Upstream:** A `NetworkException` from `network->tick()` unwinds to the catch outside the loop body (`src/frontend/stmclient.cc:562`), which sets the overlay network error and sleeps. The send-error block that would clear it (`src/frontend/stmclient.cc:553`) is skipped for that pass, so the error survives to the next frame.
* **Port:** `MoshCore::tick()` catches the exception inline and sets the overlay network error (`win32/mosh_core.cc:400`), then falls through into the send-error block in the same call. When `get_send_error()` is empty — the normal case for a `tick()` throw — the `else` branch calls `clear_network_error()` (`win32/mosh_core.cc:417`), erasing the message before `next_frame()` ever runs.
* **Consequence:** Transport errors raised by `tick()` are never shown to the user. Upstream displays them. This is a silent loss, not a timing difference: the notification is set and cleared within one function call.
* **Reachable producer:** `Connection::send()` calls `hop_port()` on the client when *both* the last port choice and the last successful round trip are older than `PORT_HOP_INTERVAL` (10 s) — `src/network/network.cc:483`. `hop_port()` constructs a `Socket`, whose constructor throws `NetworkException` on `socket`, `ioctlsocket`, or `setsockopt` failure (`src/network/network.cc:159`). Note the second condition: a healthy client whose acknowledgements keep arriving never hops, so this fires only after roughly ten seconds without a successful round trip — when the connection is already in trouble and the user most needs to be told. That is also why an empty `send_error` is the normal case for a `tick()` throw: the `sendto` on that pass already succeeded, and the failure happens in the port hop afterwards.
* **Class:** `DEFECT`, medium.
* **Note:** distinct from A6 (receive-path exceptions, which do render) and A7 (crypto exceptions, which concern the delay rather than the message). This is the tick path's message being destroyed by the send-error bookkeeping that follows it.
* **Verification:** inject a socket-creation failure at a port hop and assert the resulting notification survives into the next frame.

#### A7. A nonfatal crypto exception on the tick path requests a delay upstream does not

* **Upstream:** Displays a nonfatal `CryptoException` and begins the next pass immediately; its crypto catch contains no sleep (`src/frontend/stmclient.cc:572`).
* **Port:** Sets `retry_network_tick` and raises the returned wait time to at least 200 ms (`MoshCore::tick`, `win32/mosh_core.cc:427`).
* **Consequence:** The requested floor is not what the process actually waits. `run_loop()` caps the returned timeout at `RESIZE_POLL_CAP_MS` (100 ms) and any ready handle shortens it further, so the observable effect is that an otherwise shorter idle wait is raised to at most 100 ms. It is neither upstream's behavior (no delay) nor upstream's network-exception backoff (an unconditional 200 ms sleep).
* **Class:** `DEFECT`, low.
* **Fix:** A6 and A7 should be decided together — whether parity requires upstream's full-loop 200 ms pause or a network-only retry deadline. The current code splits the difference in a way that matches neither.

### Resize, prediction, and frames

#### I5. Resize is polled rather than SIGWINCH-gated

* **Upstream:** Installs SIGWINCH and calls `process_resize()` only when that signal is consumed (`src/frontend/stmclient.cc:236`).
* **Port:** Queries `GetConsoleScreenBufferInfo` after every wakeup — at most every 100 ms when idle, because of the `RESIZE_POLL_CAP_MS` cap — and calls `core.resize()` on a dimension change (`win32/console_io.cc:1187`).
* **Consequence:** Resize detection is delayed by up to the polling interval and costs a console round trip on idle wakeups.
* **Class:** `PLATFORM`, low. There is no SIGWINCH equivalent for the native console.

#### A8. During shutdown the port skips the prediction reset

* **Upstream:** `process_resize()` guards only the `push_back` of the resize instruction on `!shutdown_in_progress()`; it calls `overlays.get_prediction_engine().reset()` unconditionally (`src/frontend/stmclient.cc:418`).
* **Port:** `MoshCore::resize()` returns before both the enqueue and the reset when `finished` or `shutdown_in_progress()` (`win32/mosh_core.cc:378`).
* **Consequence:** A resize detected after graceful shutdown begins leaves stale local prediction overlays on screen until exit, where upstream invalidates them.
* **Class:** `DEFECT`, low.

#### A9. Initial and per-iteration frame timing differ

* **Upstream:** Writes a full empty frame before entering its first select (`src/frontend/stmclient.cc:256`), and writes a frame before computing readiness on every pass thereafter (`src/frontend/stmclient.cc:450`).
* **Port:** Writes the open sequence during setup (`win32/console_io.cc:887`) but calls `core.next_frame()` only after the wait and after dispatch (`win32/console_io.cc:1223`).
* **Consequence:** Initial screen initialization waits for the first wakeup — normally up to 100 ms — and there is no pre-wait frame emission.
* **Class:** `DEFECT`, low.

### Termination, errors, and exit

#### I6b. Only close is an interactive control-handler deadline

* **Upstream:** Has no equivalent; SIGHUP delivery imposes no kernel-enforced deadline on the handler's effects.
* **Port:** `console_control_handler` maps `CTRL_CLOSE_EVENT` to `ShutdownCause::CTRL_CLOSE`, publishes a deadline, and waits for the restoration attempt to complete (`win32/console_io.cc:609`, `win32/console_io.cc:617`, `win32/console_io.cc:630`, `win32/console_io.cc:635`).
* **Windows contract:** Microsoft's [HandlerRoutine documentation](https://learn.microsoft.com/windows/console/handlerroutine) gives `CTRL_CLOSE_EVENT` the `SPI_GETHUNGAPPTIMEOUT` budget (table value 5000 ms). It also states that `CTRL_LOGOFF_EVENT` and `CTRL_SHUTDOWN_EVENT` are received only by services: interactive applications are terminated before those signals are sent. Microsoft's [SetConsoleCtrlHandler documentation](https://learn.microsoft.com/windows/console/setconsolectrlhandler) adds that loading `gdi32.dll` or `user32.dll` suppresses those events and directs such applications to use a hidden window's `WM_QUERYENDSESSION` and `WM_ENDSESSION` messages. Thus an interactive `mosh.exe` can receive this control-handler deadline only for `CTRL_CLOSE_EVENT`.
* **Consequence:** Close must prioritize *local* console restoration over remote acknowledgement; an arbitrarily long protocol shutdown is not available.
* **Class:** `PLATFORM`, medium.
* **Design:** the handler signals the owner and blocks on a bounded completion event; see [The close control handler waits for the restoration attempt](#the-close-control-handler-waits-for-the-restoration-attempt) for why the wait is mandatory.

#### A23. Session end has no producer

* **Upstream:** SIGTERM and SIGHUP start network shutdown when a remote address exists (`src/frontend/stmclient.cc:508`).
* **Port:** Defines `ShutdownCause::SESSION_END` and a cross-thread `request_shutdown()` entry point (`win32/console_io.h:67`, `win32/console_io.cc:1053`), but no hidden window or message pump publishes that cause from `WM_QUERYENDSESSION` or `WM_ENDSESSION`.
* **Consequence:** On logoff, the client has no protocol-shutdown path and the remote `mosh-server` can be left running.
* **The producer cannot be scoped to `ConsoleSession`.** A session end can arrive during the SSH bootstrap — after `mosh-server` has started and returned its `MOSH CONNECT` reply, but before a `ConsoleSession` exists to receive `request_shutdown()`. In that window the notification has nothing to reach: the spawned `ssh` child is not reaped, the acquired key is not scrubbed, and the remote server is orphaned with no client that ever connected. Whatever owns the hidden window therefore has to outlive and precede the console session, and has to know which phase — bootstrapping, running, restoring — it is interrupting.
* **Correcting this is not simply wiring the two messages to `request_shutdown()`.** `WM_QUERYENDSESSION` is a query that may be refused or cancelled, and a cancelled session end is followed by `WM_ENDSESSION` with `wParam == FALSE`. Transport shutdown is irreversible, so publishing on the query would destroy a session the user just kept. Only a committed `WM_ENDSESSION` may publish, exactly once. The attainable guarantee is a bounded best-effort shutdown attempt before the callback returns, not a stopped remote server: Windows may terminate the process once the callback returns, and the message budget for session end is not the `SPI_GETHUNGAPPTIMEOUT` value that governs `CTRL_CLOSE_EVENT`.
* **Class:** `DEFECT`, medium. Windows supplies session-end notification through hidden-window messages; the missing producer is not platform-forced.

#### I6c. Console restoration is not reliable during close handling

* **Windows contract:** Microsoft's [HandlerRoutine documentation](https://learn.microsoft.com/windows/console/handlerroutine) states that console functions "may not work reliably" while processing `CTRL_CLOSE_EVENT`, `CTRL_LOGOFF_EVENT`, or `CTRL_SHUTDOWN_EVENT`, because console cleanup may already have run before the handler executes.
* **Port:** The close path attempts to restore input and output modes before it signals `restored` (`Impl::restore`, `win32/console_io.cc:967`, `win32/console_io.cc:982`; `Impl::release_and_signal`, `win32/console_io.cc:909`, `win32/console_io.cc:918`, `win32/console_io.cc:920`). The completion event means only that the attempt completed; the cleanup report records whether it succeeded (`win32/mosh_main.cc:173`).
* **Consequence:** Even when the handler waits within the close deadline, restoration can fail. The deadline machinery can bound the attempt; it cannot guarantee a restored console. Any user-visible cleanup report is best-effort because console output is itself among the operations documented as unreliable during close handling. Acceptance criteria for close must distinguish an attempt completing from restoration succeeding.
* **Class:** `PLATFORM`, medium.

#### A24. A blocked console write can consume the whole close deadline

* **Upstream:** No analogue. POSIX imposes no forced-termination deadline on the client, so a slow terminal write delays shutdown without cutting it short.
* **Port:** Every console write is a synchronous `WriteFile` on the owner thread — the open sequence during construction (`win32/console_io.cc:887`) and each frame in the loop (`win32/console_io.cc:1223`). Publishing a close deadline signals an event; it does not interrupt a write already in progress, and nothing cancels one.
* **Consequence:** If close arrives while the owner is inside a console write, the owner cannot reach `release_and_signal()`. The handler's wait expires, Windows terminates the process, and the console is left raw with no restoration *attempted* — the outcome the bounded close path exists to prevent. This is distinct from I6c, where the attempt runs and can fail; here it never starts. The `deadline-wedged-reader` mode does not cover it: it wedges the reader after the loop has regained control, not the writer, and not during construction.
* **Class:** `DEFECT`, medium. A5 records the same unbounded write as a latency cost; this is its termination consequence.
* **Fix:** Not chosen. Bounding it needs a cancellable output path — a dedicated writer whose handle can be cancelled, with a bounded join and a rule forbidding writes after restoration — or the close contract has to be stated as best-effort rather than bounded. That is a design decision, not a patch.
* **Verification:** a close arriving during the open sequence and during an in-progress frame write both reach a restoration attempt within the deadline.

#### A12. Send errors become sticky final status messages

* **Upstream:** Shows `get_send_error()` as a transient overlay network error, clears it, and clears the overlay when no error remains (`src/frontend/stmclient.cc:553`).
* **Port:** Does the same overlay work but also copies every send error into `impl->status` (`MoshCore::tick`, `win32/mosh_core.cc:413`), which `mosh_main` prints at session end (`win32/mosh_main.cc:168`).
* **Consequence:** A transient send error that fully recovered is still printed to stderr when the session later exits.
* **Class:** `DEFECT`, low. `status` is never cleared on recovery.

#### A13. Escape-shutdown wording and the pre-connect message differ

* **Upstream:** Ctrl-^ `.` shows `Exiting on user request...`, starts shutdown only when connected, and otherwise returns false into the caller's pre-connect break path (`src/frontend/stmclient.cc:344`).
* **Port:** `begin_shutdown()` shows `Exiting...`, and with no remote address sets the status `Exiting before connecting to server.` and finishes (`win32/mosh_core.cc:294`).
* **Consequence:** The on-screen message differs, and the port prints a terse status where upstream prints its detailed firewall and UDP diagnostic (see A21).
* **Class:** `DEFECT`, low.

#### A16. The escape-suspend sequence does nothing

* **Upstream:** Ctrl-^ followed by Ctrl-Z closes the display, restores termios, prints `[mosh is suspended.]`, raises `SIGSTOP`, and calls `resume()` on continuation (`src/frontend/stmclient.cc:353`).
* **Port:** `feed_input` matches byte `0x1a` after the escape prefix and deliberately does nothing (`win32/mosh_core.cc:252`).
* **Consequence:** A user typing the documented suspend sequence gets no feedback and no suspension; the bytes are swallowed. Upstream's own console-facing behavior — restoring the terminal and printing a message — has no Windows counterpart even though it is achievable.
* **Class:** `DEFECT`, low.
* **Note:** Windows has no `SIGSTOP`, so full parity is not available. The current no-op is not the only option: the sequence could be rejected with a notification, or mapped to a console-appropriate action. The comment at that line explains why `SIGSTOP` is absent but not why silence is the right substitute.

#### A14. The final cleanup frame is omitted

* **Upstream:** `STMClient::shutdown()` clears the notification string and title prefix, marks the server heard, and renders one final frame before restoring the terminal (`src/frontend/stmclient.cc:204`).
* **Port:** Restores the console and emits `close_sequence()` without a final `next_frame()` (`win32/console_io.cc:967`, `win32/console_io.cc:972`).
* **Consequence:** The last rendered screen retains whatever overlay state — notifications, prediction underlines — was live when the loop exited.
* **Class:** `DEFECT`, low.

#### A21. Exit-time diagnostics and the exit banner are omitted

* **Upstream:** After restoring the terminal, prints either detailed initial-connection troubleshooting (firewall, UDP port range, `-p`) or a warning that `mosh-server` may still be running (`src/frontend/stmclient.cc:220`), and finally `[mosh is exiting.]` (`src/frontend/mosh-client.cc:215`).
* **Port:** `mosh_main` prints only `status_message()` when nonempty (`win32/mosh_main.cc:168`); there is no banner.
* **Consequence:** A failed initial connection gives no firewall or UDP guidance, an unclean exit gives no server-still-running warning, and a clean exit has no banner.
* **Class:** `DEFECT`, medium — the connection-failure guidance is the single most useful diagnostic upstream prints, and this is the platform where UDP is most likely to be firewalled.
* **Note:** split from A14 because the owners differ. A14 is a frame the session emits; this is text `main` prints after restoration.
* **Caution when implementing:** upstream's troubleshooting text recommends the `-p` option for selecting a UDP port. `mosh.exe` has no port selection of any kind — the endpoint comes from the server's `MOSH CONNECT` reply (I7) — so that sentence has no analogue and must be omitted rather than reworded. Do not reintroduce a positional port to give it one; whether client-side port selection should exist at all is a separate question this finding does not settle.

---

## Confirmed parity

#### Connection timeout enters graceful shutdown

* **Upstream:** After more than 15,000 ms without a remote state, sets the notification and calls `network->start_shutdown()` (`src/frontend/stmclient.cc:539`); the transport then bounds shutdown by 16 packets or 10 seconds (`src/network/transportsender-impl.h:373`).
* **Port:** Uses the same `CONNECTION_TIMEOUT = 15000` (`win32/mosh_core.cc:64`) and calls `network->start_shutdown()` after setting the notification (`MoshCore::Impl::update_lifecycle`, `win32/mosh_core.cc:184`, `win32/mosh_core.cc:187`).
* **Status:** parity. The 15-second detection threshold and the transition into the bounded shutdown protocol match.

#### Reader input ending enters graceful shutdown

* **Upstream:** `process_user_input()` returns false for EOF and a read error (`src/frontend/stmclient.cc:316`); the main loop then breaks if not yet connected, or calls `network->start_shutdown()` if connected (`src/frontend/stmclient.cc:490`).
* **Port:** `Reader::read_loop` marks a `ReadFile` failure as input ended (`win32/console_io.cc:358`); the owner observes `has_ended()` and begins graceful shutdown with `ShutdownCause::IO_LOSS` (`win32/console_io.cc:1178`). A successful zero-byte result follows the same path (`win32/console_io.cc:370`).
* **Status:** read-failure parity. Treating a successful zero-byte console read as end-of-input is engineering judgment, not a documented Windows EOF guarantee. Microsoft's [ReadFile documentation](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-readfile) specifies end-of-file behavior for file reads but not console handles. Microsoft's [high-level console input documentation](https://learn.microsoft.com/windows/console/high-level-console-input-and-output-functions) instead says that, with line input disabled, `ReadFile` on console input does not return until at least one character is available. A successful zero-byte console read is therefore undocumented in both directions. The chosen teardown avoids an unbounded immediate retry spin: a spurious zero costs a clean shutdown; a wrong backoff can consume a core indefinitely.

#### An interrupt control event enters protocol shutdown; a typed Ctrl-C does not

* **Upstream:** SIGINT and SIGTERM start network shutdown when a remote address exists (`src/frontend/stmclient.cc:508`). A typed Ctrl-C is not one of them: raw mode leaves terminal signal generation off, so the byte goes to the remote.
* **Port:** Setup clears `ENABLE_PROCESSED_INPUT` (`win32/console_io.cc:844`), so a typed Ctrl-C reaches `ReadFile` as `0x03` and is forwarded as an ordinary user byte — the escape key is `0x1e` (`win32/mosh_core.cc:114`), so nothing intercepts it. A **delivered** `CTRL_C_EVENT` or `CTRL_BREAK_EVENT` maps to `ShutdownCause::CTRL_BREAK` and signals the termination event (`win32/console_io.cc:609`, `win32/console_io.cc:615`, `win32/console_io.cc:630`); the owner observes it and begins graceful shutdown (`win32/console_io.cc:1172`, `win32/console_io.cc:1070`).
* **Status:** parity for the interrupt path, on first signal. With a remote address, `MoshCore::begin_shutdown()` starts the protocol shutdown (`win32/mosh_core.cc:294`). A repeated event is idempotent: after the first observation the loop no longer polls the termination event, and `CTRL_BREAK` carries no deadline (`win32/console_io.cc:1110`, `win32/console_io.cc:1172`, `win32/console_io.cc:125`). A close event can still escalate the same shutdown by publishing its deadline.
* **What is verified and what is not:** the acceptance modes drive `request_shutdown()` directly, so they establish the cause-to-shutdown mapping and its idempotence, not the delivery of a real control event. Keyboard forwarding of `0x03` rests on the cleared mode flag, not on a test.
* **Windows-only exit path:** clearing `ENABLE_PROCESSED_INPUT` suppresses Ctrl-C but not Ctrl-Break, so a *typed* Ctrl-Break does generate `CTRL_BREAK_EVENT` and ends the session locally. Upstream has no equivalent key.

#### The close control handler waits for the restoration attempt

* **Port:** `console_control_handler` publishes the close deadline and waits for `control->restored` for the remaining OS budget (`win32/console_io.cc:609`, `win32/console_io.cc:630`, `win32/console_io.cc:635`); `Impl::release_and_signal` attempts restoration and signals that event (`win32/console_io.cc:909`, `win32/console_io.cc:918`, `win32/console_io.cc:920`).
* **Status:** parity with the required bounded close-handling contract, for the paths that reach teardown. Returning `TRUE` alone cannot preserve the restoration-attempt window: Microsoft's [HandlerRoutine documentation](https://learn.microsoft.com/windows/console/handlerroutine) says the system terminates the process when `HandlerRoutine` returns `TRUE` or when the timeout expires. A signalled `restored` event means the attempt finished, not that it succeeded; the cleanup report holds that result. Waiting is therefore required, not defensive.
* **Limit:** the handler bounds its own wait, which is not the same as bounding the owner. An owner blocked in a console write never reaches teardown at all — see A24.

#### The cached timestamp is frozen after the wait

* **Upstream:** `Select::select()` freezes the timestamp after `pselect` returns (`src/util/select.h:186`).
* **Port:** `run_loop()` calls `core.refresh_clock()` after `WaitForMultipleObjects` returns and before any dispatch (`win32/console_io.cc:1152`); `MoshCore::refresh_clock` freezes the shared timestamp (`win32/mosh_core.cc:433`).
* **Status:** parity. Inbound state timestamps and RTT samples use wakeup time in both.

#### Resize leaves the framebuffers to the remote state

* **Upstream:** `STMClient::process_resize()` queues the resize instruction and resets prediction. It does not touch any framebuffer; the server's echoed state changes the rendered dimensions (`src/frontend/stmclient.cc:418`).
* **Port:** `MoshCore::resize()` queues the instruction and resets prediction, and leaves `local_framebuffer` and `new_state` alone (`win32/mosh_core.cc:382`).
* **Status:** parity. Reallocating the framebuffers would discard the diff baseline and force a full repaint of the still-old-sized remote state, followed by a second repaint when the server echoes the resize. Remote state is now the sole authority for framebuffer dimensions in both.
* **Note:** the shutdown-path guard in front of both statements remains a divergence — see A8.

#### UCRT wide-printf semantics

* **Upstream:** Uses `%s` in wide `swprintf` calls for narrow strings (`src/frontend/stmclient.cc:199`).
* **Port:** Matching formats use `%s` (`win32/mosh_core.cc:80`), with the Windows build configured for POSIX wide-stdio semantics.
* **Status:** parity. UCRT's interpretation could otherwise garble or read past narrow strings; this is a CRT conformance issue, not a state-machine difference.

#### No supported invocation puts the session key on `mosh.exe`'s command line (S1, repaired)

* **Upstream:** `mosh-client` reads the key from `MOSH_KEY` (`src/frontend/mosh-client.cc:167`) and immediately calls `unsetenv( "MOSH_KEY" )`, exiting if the unset fails (`src/frontend/mosh-client.cc:183`).
* **Port:** The sole invocation is `mosh.exe <user@host>`, which obtains the key from the server's `MOSH CONNECT` reply over the `ssh` pipe. An endpoint is never accepted positionally: `classify_invocation` routes every argument shape other than a single destination to usage (`win32/mosh_bootstrap.cc:365`, `win32/mosh_main.cc:140`), so no supported invocation asks a user to put a key there and nothing in the program reads one from `argv`.
* **The property, stated so it can be enforced:** no interface requires or interprets a session key from `argv`; the server-issued key first exists after every child command line has been built (`build_ssh_command_line` takes only the ssh path and the destination, `win32/mosh_bootstrap.cc:164`, and is called before the reply is read, `win32/mosh_bootstrap.cc:382`); and `mosh.exe` never writes the key back into an argument, an environment block, or a diagnostic of its own — the child inherits stderr, so what `ssh.exe` prints is the child's to answer for, and S2 disposes of that child. What the program cannot do is keep a caller from typing a secret into the destination argument itself — that string is untyped, and it is copied into both this command line and `ssh.exe`'s. Argument classification governs what the program *accepts and reads*, not what a caller can place in a process parameter block that Windows populates before `main` runs.
* **What this record does not cover:** only `mosh.exe`'s own command line. `ssh.exe` necessarily holds the key too — it decrypts and buffers the `MOSH CONNECT` line before writing it to the pipe — and it is selected from `PATH`, so its provenance and its crash behavior are part of the same trust boundary and are governed by neither this repair nor anything `mosh.exe` can establish about a child it did not build. S2 now names it and disposes of it: a PATH-resolved OpenSSH client is a trusted external dependency, with the residual risk accepted.
* **Status:** parity on the property that matters — no supported path puts the session key where a Windows process parameter block would hold it, because such a block cannot be cleared once read. The mechanism differs deliberately. Upstream's environment channel does not transfer: a user who sets `MOSH_KEY` in a shell leaves the key in a long-lived parent the client cannot reach, and a child cannot establish dump protection before its own environment block exists, so the key is present during loader and startup failures. The pipe handoff avoids both.
* **Verification:** the classifier rejects endpoint-shaped argument lists (`win32/test_bootstrap.cc:322`), and CI asserts that the binary it builds refuses such an invocation with the usage code and that its usage text advertises no key, prediction mode, or positional endpoint. The CI probe uses a *valid* 22-character key, because `Base64Key` rejects a malformed one and an invocation refused by key validation would look identical to one refused by argument classification.
* **What the verification proves:** that the retired syntax stays retired. It is a regression guard, not a proof that no credential can reach a command line — it would not catch a future `--key` option, a differently named credential argument, or a key interpolated into some later child's command line. The invariants that would need their own checks are that the accepted grammar contains only a destination, and that every child command line is built before the server-issued key exists. The check with the right shape is a fixture that plants a unique sentinel in place of the server's key, records every spawned child's command line and environment block, captures everything `mosh.exe` writes, and asserts the sentinel appears only on the path that consumes it. Routing all production process creation through one launcher would make that assertion hold for children added later, instead of only the one that exists now.
* **If a direct-endpoint facility is ever wanted again:** the retired one was advertised in the usage text, so removing it is a deliberate breaking change and not the deletion of unreachable development code. It never reached a release, which is the evidence that matters and is stronger than "no consumer is known": the fork publishes no releases, and the only tag carrying a `win32/` tree is `attic/task-4a-v1`, a development snapshot that does advertise the form (`win32/mosh_main.cc:72` there). Tagged, then, but never shipped. One workflow still left with it. Supplying an endpoint directly was the only way to reach a host whose server-side `SSH_CONNECTION` address the client cannot route to — behind NAT or a load balancer — since `mosh.exe <user@host>` derives its UDP target from that address. Those topologies are out of scope by decision, recorded under I9; the workflow is not replaced, it is withdrawn. A jump-only host is a different case with a different status: refused by the pinned proxy options, policy unresolved, tracked as I10. What the removal genuinely costs nothing is the rest: an ordinary session is `mosh.exe <user@host>`, and a synthetic endpoint is constructed in-process by the unit tests, which never build a command line. Restoring that reachability would not require restoring this interface, and the two should not be conflated: an address is routing metadata, only the key is secret. An endpoint override that keeps the ssh bootstrap for the key, or upstream's own client-visible-address discovery (`local` or `proxy`), would serve the withdrawn topologies with nothing secret in `argv` at all. I9 holds what such an override would have to cover. That is the shape to reach for if the scope decision is ever revisited. What follows applies only to the narrower case of a session established with no bootstrap at all. Should such a facility become necessary, it must take the key over a channel whose contents the receiving process can bound: an inherited pipe, or an equivalent handoff that happens after the process has started. Not an environment variable, not an interactive paste, and not another command-line spelling. The command line is the settled one: whatever its spelling, it lands in a process parameter block that cannot be cleared once read, which is precisely what this record repaired — a `--key` option would reintroduce it exactly. The other two are ruled out under the threat model this repair assumed — a same-user reader — rather than absolutely. An environment variable inherited from the user's shell sits in a long-lived parent the client cannot scrub, and is present in the child's own block during loader and startup failures, before the child can protect itself; an ephemeral launcher would avoid the first half of that but not the second. An interactive paste puts the key through console input and screen buffers whose lifetime the reader does not control by default; no-echo input narrows that, and a design that relies on it has to say so and show it. The channel is the constraint; the protocol is not designed here. A real proposal would still have to settle who obtains the key, how the child identifies the handle it was given, handle-inheritance limits, cancellation and timeout, and zeroization on both sides — none of which can be chosen sensibly before a consumer exists.
* **Residual:** the bootstrap's intermediate key copies — the parsed reply and the pipe drain buffers — are freed without being zeroed, so a crash-dump-class adversary can still recover the key from freed heap. This is not a divergence: upstream's `mosh-client` zeroes none of its key copies either. Crash-dump exposure itself is S2, which is unaffected by this repair.

## Seeds checked against the code

* `CONNECTION_TIMEOUT = 15000` is not an invented value. Upstream uses the same literal (`src/frontend/stmclient.cc:539`). The threshold and transition match.
* Timestamp placement is current parity, not a divergence.
* The resize seed's direction was backwards. It is the **port** that skips the prediction reset during shutdown, not upstream — see A8.
