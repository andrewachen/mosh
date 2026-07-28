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
| `PLATFORM` | 6 |
| `POLICY` | 1 |
| `DEFERRED` | 3 |
| `DEFECT` | 22 |
| `OPEN` | 1 |
| **Total** | **33** |

Two previously repaired divergences — post-wait timestamp freezing and UCRT wide-printf semantics — are recorded at the end as confirmed parity, and are not counted as findings.

## How the defects cluster

Six are **configuration omissions**, each independent and individually cheap: `MOSH_ESCAPE_KEY` (A1), `MOSH_PREDICTION_OVERWRITE` (A2), the `[mosh] ` title prefix (A3), `-v` diagnostics (A4), `MOSH_NO_TERM_INIT` (A20), and the escape-suspend sequence (A16).

Eleven are **event-loop and shutdown drift**: A5, A6, A7, A8, A9, A12, A15, A17, A18, A19, A22, plus the three `DEFERRED` items. `STMClient::main()` was re-derived rather than extracted, so every ordering and exception-boundary decision was re-made independently, and each drifted on its own.

Whether the correct remedy is a shared platform-neutral loop coordinator (taking normalized events, returning actions and deadlines) or platform-specific loops held together by parity tests is an open architectural question — a literal extraction of `STMClient::main()` is unlikely to stay simple, because the POSIX and Win32 waiting, input, resize, and termination contracts genuinely differ.

What must be settled first is the **behavioral contract**, not the code organization: the intended phase ordering, exception boundaries, timer semantics, fairness limits, and shutdown invariants, recorded as expected event traces rather than prose — ordinary input, simultaneous input and network readiness, receive error, send error, crypto error, resize during shutdown, first Ctrl-C, second Ctrl-C, and close or logoff termination. Those traces serve either architecture and make the eventual coordinator decision evidence-based. Sharing an implementation stays an evaluated option, not a prerequisite.

Only the genuinely coupled findings wait on that contract: A5 and A9 (phase ordering) and A6, A7, and A15 (exception boundaries and retry timing). A18 is an independent safety invariant with a local contract test and should land now — it is the one high-severity item here, and holding a resource-exhaustion fix behind a speculative refactor is the wrong trade. A22 is local error-state bookkeeping and is likewise independently fixable.

The remaining defects are exit-path omissions (A13, A14, A21) and two security defects (S1, S2).

### The host loop, for reference

Several findings below depend on the exact phase order, so it is stated once here. `ConsoleSession::Impl::run_loop()` (`win32/console_io.cc:867`) runs:

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
* **Port:** Constructs `Display(false)` and delegates raw mode, UTF-8 code pages, and VT output to `ConsoleSession` (`win32/mosh_core.cc:112`, `win32/console_io.cc:655`).
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
* **Port:** Accepts only a display-preference argument and never calls `PredictionEngine::set_predict_overwrite()` (`win32/mosh_core.cc:121`, `win32/mosh_main.cc:128`).
* **Consequence:** Insert and delete prediction can visibly differ from upstream before the server echo arrives.
* **Class:** `DEFECT`, low. The shared prediction engine already exposes the operation.

#### A3. The `[mosh] ` title prefix is never set

* **Upstream:** `STMClient::init()` sets the prefix unless `MOSH_TITLE_NOPREFIX` is set (`src/frontend/stmclient.cc:125`); `STMClient::shutdown()` clears it before the final frame (`src/frontend/stmclient.cc:209`).
* **Port:** Never sets or clears an overlay title prefix (`win32/mosh_core.cc:111`).
* **Consequence:** Remote title updates are not marked as mosh titles during the session.
* **Class:** `DEFECT`, low. `Display(false)` still has title support (`src/terminal/terminaldisplayinit.cc:83`), so this is not forced by the display choice.
* **Note:** Upstream's shutdown *clears* the prefix; it never saves or restores whatever title existed before mosh started. Because the port never adds a prefix, both implementations leave the same bare remote title after exit. Only the in-session prefix differs.

#### A4. Upstream's `-v` diagnostics are unavailable

* **Upstream:** Parses repeatable `-v` (`src/frontend/mosh-client.cc:129`) and calls both `network->set_verbose()` and `Select::set_verbose()` (`src/frontend/stmclient.cc:260`).
* **Port:** Accepts only `<ip> <port> <key> [predict]` (`win32/mosh_main.cc:93`) and never sets transport verbosity (`win32/mosh_core.cc:149`).
* **Consequence:** Neither transport diagnostics nor poll diagnostics can be requested, making field troubleshooting harder.
* **Class:** `DEFECT`, low.
* **Fix:** Two tiers with very different costs. Transport verbosity is a flag plus one existing setter. Reproducing `Select`'s poll diagnostics has no shared setter to call and requires instrumenting the Win32 loop; see A18, which covers the behavior those diagnostics describe.

#### I7. The executable is a standalone CLI, not a wrapper-invoked client

* **Upstream:** `mosh-client` is wrapper-facing: it parses `-c`/`-v`, reads prediction settings from the environment, and returns `!success` (`src/frontend/mosh-client.cc:129`).
* **Port:** A standalone executable taking IP and port as positional arguments, returning distinct `2`, `3`, or `4` for usage, frontend, and exception errors (`win32/mosh_main.cc:105`).
* **Consequence:** Scripts written against `mosh-client` do not invoke `mosh.exe` compatibly, and error classes carry different numeric statuses.
* **Class:** `POLICY`, low. `mosh.exe` is deliberately standalone and is not required to mirror `mosh-client`'s invocation or exit statuses.
* **Scope:** This finding covers invocation shape and exit status only. Credential transport is a separate question and is recorded as S1 — "standalone" does not imply "key on the command line."

#### S1. The session key is passed as a command-line argument

* **Upstream:** Reads the key from `MOSH_KEY`, copies it into a `std::string`, and immediately calls `unsetenv( "MOSH_KEY" )`, failing hard if the unset fails (`src/frontend/mosh-client.cc:168`).
* **Port:** Takes the key as `argv[3]` and passes it straight to the `MoshCore` constructor (`win32/mosh_main.cc:129`). The usage string documents it as a positional argument (`win32/mosh_main.cc:95`).
* **Consequence:** Windows retains the full command line in the process parameter block for the process lifetime. The key is therefore readable by same-user and administrator process inspection, and can reach diagnostic tooling, telemetry, and crash dumps. Unlike upstream's environment variable, a command line cannot be cleared after it is read.
* **Class:** `DEFECT`, **high** — security. Being a standalone executable does not require putting the key in `argv`.
* **Fix:** Move the key off the command line. The remedy is not yet chosen, and the candidates differ materially. An inherited anonymous pipe or handle is the strongest channel but presumes a launcher, which I7 says is not part of the product shape — choosing it means also specifying how a direct user invokes `mosh.exe`. A no-echo interactive read covers the direct-user case without a launcher. Either way the existing positional form has to be retired, not kept alongside.
* **Why upstream's environment pattern does not transfer unchanged:** the property upstream actually gets from `exec`ing the client is narrow — no *separate long-lived parent* retains the key. It is not erasure. The replacement process still starts with the key in its initial environment block, and `unsetenv` removes the entry from `environ` without overwriting those bytes, so `/proc/<pid>/environ` can keep exposing it. On Windows a user who types `set MOSH_KEY=...` before running `mosh.exe` loses even that narrow property: the key stays in a long-lived parent shell `mosh.exe` cannot reach, and that parent passes it to every later child. Environment input is therefore acceptable only from an ephemeral launcher supplying a scoped process-creation environment block, and even then it is a deliberately weaker compatibility mode.
* **The environment path cannot satisfy S2's ordering requirement.** Windows maps the process parameter block before the entry point, CRT initialization, or any dump-protection call, so the key is in child memory during loader and startup failures — a window that clearing the views later cannot close. Either drop environment transport from the protected paths, or require a launcher handshake in which the child establishes dump policy, reports readiness, and only then receives the key over a tightly inherited pipe. If the weaker mode is kept, state plainly that pre-entry-point exposure is outside its guarantee; do not claim it meets the same fail-closed release invariant.
* **No-echo interactive input needs its own limits specified:** behavior with no console, with redirected stdin, on EOF and cancellation, whether paste is supported, and whether manual acquisition leaves the key in remote-server output, scrollback, or the clipboard. It solves command-line and screen echo exposure; it is not automatically equivalent to an in-memory handoff.
* **Acceptance criteria:** the key does not appear in `GetCommandLineW()`, in the usage string, in any spawned child's command line, in any diagnostic or error output, in the invoking process's environment after startup, or — if the environment mode is retained — in the child's own Win32 or CRT environment views after startup, which must also be non-inheritable by descendants. Source precedence, handle-inheritance restrictions, parent and child lifetimes, ownership, and zeroization after use are specified before the change lands. Crash-dump exposure is a separate defect — see S2.

#### S2. Core-dump protection is a no-op and is never called

* **Upstream:** `mosh-client` calls `Crypto::disable_dumping_core()` as one of the first statements in `main()`, before it reads `MOSH_KEY` (`src/frontend/mosh-client.cc:112`); `mosh-server` does the same (`src/frontend/mosh-server.cc:181`). The implementation sets `RLIMIT_CORE` to zero and exits on failure (`src/crypto/crypto.cc:292`).
* **Port:** `win32/mosh_main.cc` never calls it, and the entire function body is inside `#ifndef _WIN32` (`src/crypto/crypto.cc:294`), so calling it would do nothing. The Windows build has no equivalent protection.
* **Consequence:** A crash after the key is in memory can persist it to disk through Windows Error Reporting or any other minidump path. This is independent of S1: moving the key off `argv` removes it from the process parameter block but leaves it in the heap that a dump captures.
* **Class:** `DEFECT`, **high** — security. `BUILD-CLANGARM64.md` already records the missing Windows counterpart as a release blocker; this finding exists so the parity inventory does not imply the security surface is covered by S1 alone.
* **Fix:** define and implement the Windows behavior the no-op stands in for, rather than only gating the shim out of production builds. "Suppress crash dumps" is not by itself an implementable requirement: normal WER reporting, administrator-configured LocalDumps, application-installed crash handlers, attached debuggers, and external processes calling the dump APIs are distinct producers, and an unprivileged process cannot categorically stop all of them — just as `RLIMIT_CORE=0` does not stop a privileged debugger from reading process memory. Enumerate the producers and state for each whether it must be prevented, must fail startup, requires installer or system policy, or is outside the threat model. Scope parity to the automatic crash persistence the process itself controls, call the protection before the key is acquired, and fail closed when the promised protection cannot be established.
* **Verification:** a deliberately crashing helper under each supported WER and LocalDumps configuration, asserting no dump containing the key is written for the producers declared in scope.
* **Related:** dump suppression and zeroization are complementary, not interchangeable. Suppressing one dump path does not remove residual copies left by uncontrolled `std::string` ownership, and end-of-session clearing does nothing for a mid-session crash. Only the *expanded* cipher context is inherently session-lived: `Session::Session` passes `key.data()` to `ae_init()` and every later encrypt and decrypt uses `ctx` (`src/crypto/crypto.cc:161`), so the encoded and decoded bootstrap copies can be zeroed immediately after initialization rather than held for the session. Specify which objects own the encoded key, the decoded key, and the expanded state, and when each is cleared.

### Event loop, timing, and network dispatch

#### I2. Win32 waits on handles rather than `pselect` fds and POSIX signals

* **Upstream:** Registers network fds and `STDIN_FILENO` and `pselect`s them, consuming SIGWINCH, SIGCONT, SIGTERM, SIGINT, SIGHUP, and SIGPIPE (`src/frontend/stmclient.cc:447`). `Select::select()` atomically unblocks signals for the wait (`src/util/select.h:143`).
* **Port:** Waits with `WaitForMultipleObjects` on the reader event, the termination event, and `WSAEventSelect(FD_READ)` events (`win32/console_io.cc:890`); the console handler translates control events into the termination event (`console_control_handler`, `win32/console_io.cc:473`).
* **Consequence:** Control events and socket readiness are dispatched through handles rather than signals and fds.
* **Class:** `PLATFORM`, low.

#### I3. Reader thread, CESU-8 conversion, bounded queue, and fair dispatch replace `read(stdin)`

* **Upstream:** Reads up to 16 KiB synchronously from `STDIN_FILENO` once select reports it readable, then processes every returned byte in that pass (`STMClient::process_user_input`, `src/frontend/stmclient.cc:310`).
* **Port:** A worker blocks in `ReadFile`, recombines conhost CESU-8 surrogate pairs, and queues up to `INPUT_QUEUE_CAP_BYTES` (4 MiB); the owner consumes at most `INPUT_BUDGET_BYTES` (64 KiB) per wakeup (`Reader::read_loop`, `win32/console_io.cc:272`; `win32/console_io.cc:960`).
* **Consequence:** A large paste is deliberately spread across loop iterations, so termination, resize, and socket work stay responsive; Windows input encoding is normalized before the shared parser sees it.
* **Class:** `PLATFORM`, low. Console `ReadFile` does not provide selectable byte-stream semantics, and an unbounded drain would starve the other handle sources.

#### I4. One readable socket is serviced per wakeup

* **Upstream:** Inspects every fd in the ready set but records only a boolean, invoking `process_network_input()` once per loop (`src/frontend/stmclient.cc:476`).
* **Port:** Re-tests the snapshot's WSA events, calls `core.on_readable()` for the first `FD_READ`, then breaks because the receive path can prune sockets (`win32/console_io.cc:944`).
* **Consequence:** Both process one transport receive action per ordinary wakeup; the port additionally avoids dereferencing a snapshot invalidated by pruning.
* **Class:** `PLATFORM`, low.

#### A18. The zero-timeout poll throttle is absent

* **Upstream:** `Select::select()` counts consecutive zero-timeout calls and raises the timeout to 1 ms from the tenth onward, resetting the counter when a nonzero timeout appears (`src/util/select.h:131`, `MAX_POLLS = 10`).
* **Port:** Passes the computed timeout straight through `std::min( timeout, RESIZE_POLL_CAP_MS )` to `WaitForMultipleObjects`, so a zero stays zero indefinitely (`win32/console_io.cc:889`).
* **Consequence:** When the transport or an overlay repeatedly returns a zero wait time, the port busy-spins at full CPU where upstream throttles to 1 ms. This is a live resource-exhaustion risk, not a diagnostic difference — upstream's `-v` messages merely *report* the throttle that the same code performs.
* **Class:** `DEFECT`, **high**.
* **Fix:** Replicate the consecutive-poll counter in `run_loop()`. Verification: drive the loop with a stub that returns zero repeatedly and assert the observed wait floor.

#### A5. `tick()` runs before the wait, so frame generation sits between dispatch and transmission

* **Upstream:** Processes ready network, input, and resize work and then calls `network->tick()` in the same pass (`src/frontend/stmclient.cc:486`).
* **Port:** Calls `core.tick()` at the top of the iteration, before the wait (`win32/console_io.cc:870`); dispatch happens after the wait, and the next `tick()` is reached only after `core.next_frame()` has been generated and written (`win32/console_io.cc:944`).
* **Consequence:** Bytes entered or state received in a wakeup are transmitted on the *next* iteration's tick rather than the current pass. Because no second wait intervenes, the added delay is the cost of frame generation and console output, not a poll interval — but that cost is unbounded under console output backpressure, and it is paid before every transmission.
* **Class:** `DEFECT`, medium.
* **Note:** An earlier revision of this document claimed the resize-poll cap could add nearly 100 ms here. That was wrong: the loop returns directly to `tick()` after rendering.

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
* **Port:** `MoshCore::tick()` catches the exception inline and sets the overlay network error (`win32/mosh_core.cc:398`), then falls through into the send-error block in the same call. When `get_send_error()` is empty — the normal case for a `tick()` throw — the `else` branch calls `clear_network_error()` (`win32/mosh_core.cc:415`), erasing the message before `next_frame()` ever runs.
* **Consequence:** Transport errors raised by `tick()` are never shown to the user. Upstream displays them. This is a silent loss, not a timing difference: the notification is set and cleared within one function call.
* **Reachable producer:** `Connection::send()` calls `hop_port()` on the client when *both* the last port choice and the last successful round trip are older than `PORT_HOP_INTERVAL` (10 s) — `src/network/network.cc:483`. `hop_port()` constructs a `Socket`, whose constructor throws `NetworkException` on `socket`, `ioctlsocket`, or `setsockopt` failure (`src/network/network.cc:159`). Note the second condition: a healthy client whose acknowledgements keep arriving never hops, so this fires only after roughly ten seconds without a successful round trip — when the connection is already in trouble and the user most needs to be told. That is also why an empty `send_error` is the normal case for a `tick()` throw: the `sendto` on that pass already succeeded, and the failure happens in the port hop afterwards.
* **Class:** `DEFECT`, medium.
* **Note:** distinct from A6 (receive-path exceptions, which do render) and A7 (crypto exceptions, which concern the delay rather than the message). This is the tick path's message being destroyed by the send-error bookkeeping that follows it.
* **Verification:** inject a socket-creation failure at a port hop and assert the resulting notification survives into the next frame.

#### A7. A nonfatal crypto exception on the tick path requests a delay upstream does not

* **Upstream:** Displays a nonfatal `CryptoException` and begins the next pass immediately; its crypto catch contains no sleep (`src/frontend/stmclient.cc:572`).
* **Port:** Sets `retry_network_tick` and raises the returned wait time to at least 200 ms (`MoshCore::tick`, `win32/mosh_core.cc:425`).
* **Consequence:** The requested floor is not what the process actually waits. `run_loop()` caps the returned timeout at `RESIZE_POLL_CAP_MS` (100 ms) and any ready handle shortens it further, so the observable effect is that an otherwise shorter idle wait is raised to at most 100 ms. It is neither upstream's behavior (no delay) nor upstream's network-exception backoff (an unconditional 200 ms sleep).
* **Class:** `DEFECT`, low.
* **Fix:** A6 and A7 should be decided together — whether parity requires upstream's full-loop 200 ms pause or a network-only retry deadline. The current code splits the difference in a way that matches neither.

### Resize, prediction, and frames

#### I5. Resize is polled rather than SIGWINCH-gated

* **Upstream:** Installs SIGWINCH and calls `process_resize()` only when that signal is consumed (`src/frontend/stmclient.cc:236`).
* **Port:** Queries `GetConsoleScreenBufferInfo` after every wakeup — at most every 100 ms when idle, because of the `RESIZE_POLL_CAP_MS` cap — and calls `core.resize()` on a dimension change (`win32/console_io.cc:929`).
* **Consequence:** Resize detection is delayed by up to the polling interval and costs a console round trip on idle wakeups.
* **Class:** `PLATFORM`, low. There is no SIGWINCH equivalent for the native console.

#### A17. Resize eagerly replaces both framebuffers and forces a repaint

* **Upstream:** `STMClient::process_resize()` queues the resize instruction and resets prediction. It does not touch any framebuffer; the server's echoed state changes the rendered dimensions (`src/frontend/stmclient.cc:418`).
* **Port:** `MoshCore::resize()` additionally constructs new `local_framebuffer` and `new_state` at the new dimensions and sets `repaint_requested` (`win32/mosh_core.cc:382`).
* **Consequence:** Discarding `local_framebuffer` discards the diff baseline, so the next frame is a full repaint of the still-old-sized remote state at the new dimensions — followed by another full repaint when the server echoes the resize. The user sees two repaints and a transient mismatch instead of one clean transition.
* **Class:** `DEFECT`, medium. Independent of A8 (shutdown-path guard) and A9 (frame placement).
* **Invariant to restore:** a host resize updates transport intent and prediction state; remote state remains the sole authority for framebuffer dimensions.
* **Verification:** a dimension-changing resize does not by itself discard the diff baseline or force a full repaint, and the echoed resize produces exactly one dimension transition.

#### A8. During shutdown the port skips the prediction reset

* **Upstream:** `process_resize()` guards only the `push_back` of the resize instruction on `!shutdown_in_progress()`; it calls `overlays.get_prediction_engine().reset()` unconditionally (`src/frontend/stmclient.cc:418`).
* **Port:** `MoshCore::resize()` returns before both the enqueue and the reset when `finished` or `shutdown_in_progress()` (`win32/mosh_core.cc:378`).
* **Consequence:** A resize detected after graceful shutdown begins leaves stale local prediction overlays on screen until exit, where upstream invalidates them.
* **Class:** `DEFECT`, low.

#### A9. Initial and per-iteration frame timing differ

* **Upstream:** Writes a full empty frame before entering its first select (`src/frontend/stmclient.cc:256`), and writes a frame before computing readiness on every pass thereafter (`src/frontend/stmclient.cc:450`).
* **Port:** Writes the open sequence during setup (`win32/console_io.cc:701`) but calls `core.next_frame()` only after the wait and after dispatch (`win32/console_io.cc:965`).
* **Consequence:** Initial screen initialization waits for the first wakeup — normally up to 100 ms — and there is no pre-wait frame emission.
* **Class:** `DEFECT`, low.

### Termination, errors, and exit

#### I6a. Ctrl-C and Ctrl-Break do not enter protocol shutdown

* **Upstream:** SIGINT and SIGTERM start network shutdown when a remote address exists (`src/frontend/stmclient.cc:508`).
* **Port:** `console_control_handler` records `ShutdownCause::CTRL_BREAK` and signals the termination event (`win32/console_io.cc:477`); the owner loop breaks before resize, socket, input, and frame work (`win32/console_io.cc:922`) without calling `core.begin_shutdown()`.
* **Consequence:** Ctrl-C and Ctrl-Break end the client without sending the protocol shutdown request. The remote `mosh-server` can keep running.
* **Class:** `DEFERRED`, medium — Task 4b.
* **Why this is not platform-forced:** returning `TRUE` from the handler for `CTRL_C_EVENT` and `CTRL_BREAK_EVENT` suppresses the default handler and does *not* cause Windows to terminate the process after the callback returns. There is no OS deadline here, so the owner thread can run a full bounded protocol shutdown. Only the handoff from the OS-supplied handler thread to the owner thread is required, and that already exists.
* **Design:** hand off to the owner, enter protocol shutdown, keep servicing the network through the transport deadline, and define what a second Ctrl-C does.

#### I6b. Close, logoff, and shutdown events are bounded by an OS termination budget

* **Upstream:** Has no equivalent; SIGHUP delivery imposes no kernel-enforced deadline on the handler's effects.
* **Port:** `console_control_handler` maps `CTRL_CLOSE_EVENT`, `CTRL_LOGOFF_EVENT`, and `CTRL_SHUTDOWN_EVENT` to `ShutdownCause::CTRL_CLOSE` and returns (`win32/console_io.cc:481`).
* **Consequence:** For these three causes Windows may terminate the process shortly after the handler returns, so an arbitrarily long protocol shutdown is not available.
* **Class:** `PLATFORM`, medium.
* **Design:** these causes must prioritize *local* console restoration over remote acknowledgement. The handler should signal the owner and then block on a bounded completion event rather than returning immediately — see A19, which is the gap between that design and the current code.

#### A19. The control handler does not wait for console restoration

* **Code comments assert otherwise:** `Impl::restore_and_signal` is documented "noexcept because a control handler is blocked on restored" (`win32/console_io.cc:715`), and `run_loop`'s preamble states "A control handler blocks on this event" (`win32/console_io.cc:864`). No such wait exists — `console_control_handler` calls `SetEvent( control->termination )` and returns `TRUE` immediately (`win32/console_io.cc:495`), and `control->restored` is read only by `console_test_last_restored_event()` and `restored_event_for_test()`.
* **Consequence:** For the I6b causes, nothing prevents Windows from killing the process between the handler's return and the owner thread's restoration, leaving the console in raw mode with the alternate screen active. The restoration guarantee the surrounding code was written to uphold is not actually established. The `restored` event, its careful noexcept contract, and the "nothing that can throw or allocate may sit between" invariant are all in place and unused.
* **Class:** `DEFECT`, **high** — Task 4b is the natural vehicle.
* **Fix:** have the handler block on `control->restored` with a bounded timeout before returning. Verification: assert the handler does not return before `restored` is signalled, and that it does return once the timeout elapses.

#### A10. The connection timeout exits immediately instead of entering graceful shutdown

* **Upstream:** After more than 15,000 ms without a remote state, sets the notification and calls `network->start_shutdown()` (`src/frontend/stmclient.cc:536`); the transport then bounds shutdown by 16 packets or 10 seconds (`src/network/transportsender-impl.h:373`).
* **Port:** Uses the same `CONNECTION_TIMEOUT = 15000` (`win32/mosh_core.cc:64`) but sets `finished = true` without `start_shutdown()` (`MoshCore::Impl::update_lifecycle`, `win32/mosh_core.cc:184`).
* **Consequence:** The 15-second detection threshold matches, but the transition does not: upstream starts its shutdown protocol while the port tears down immediately and reports an unclean exit.
* **Class:** `DEFERRED`, medium — Task 4b.
* **Note for implementation:** the two user-observable timings are different quantities. "Connection declared timed out" is 15 s in both. "Process has exited" is 15 s in the port today, but adopting upstream's transition extends it by up to the shutdown retry deadline. Tests must distinguish them, and the total should be a deliberate choice rather than a side effect.

#### A11. Reader failure throws instead of initiating graceful shutdown

* **Upstream:** `process_user_input()` returns false on a read error (`src/frontend/stmclient.cc:316`); the main loop then breaks if not yet connected, or calls `network->start_shutdown()` if connected (`src/frontend/stmclient.cc:490`).
* **Port:** `Reader::read_loop` records a `ReadFile` failure and wakes the loop (`win32/console_io.cc:278`), and `check_failure()` throws `ConsoleError` (`win32/console_io.cc:927`).
* **Consequence:** Loss of console input becomes frontend exit code 3 rather than an attempted protocol shutdown with upstream's unclean-exit diagnostic.
* **Class:** `DEFERRED`, medium — Task 4b.

#### O1. A successful zero-byte console read is not treated as EOF

* **Upstream:** `read()` returning 0 is EOF and drives the same shutdown path as an error (`src/frontend/stmclient.cc:316`).
* **Port:** `Reader::read_loop` hits `if ( read == 0 ) { continue; }` and loops (`win32/console_io.cc:289`), so upstream's EOF signal produces no shutdown and no exit.
* **Class:** `OPEN`. Whether a native console handle can return a successful zero-byte read is not established. The current code silently assumes it cannot, and the assumption is undocumented.
* **To resolve:** determine from native console behavior across the supported Windows and conhost versions — including console closure and read cancellation — whether this is reachable, and if so whether a repeated zero-byte success means EOF, a transient condition, or a console-host quirk. Only then can it be classified. Held separate from A11 so a `DEFERRED` label does not imply the EOF question is already answered.
* **Second hazard, independent of the EOF meaning:** if a successful zero-byte read is reachable and returns promptly, the bare `continue` busy-spins the reader thread. Whatever the shutdown interpretation turns out to be, the resolution must be a terminal interpretation or a bounded backoff — never an unbounded immediate loop.

#### A12. Send errors become sticky final status messages

* **Upstream:** Shows `get_send_error()` as a transient overlay network error, clears it, and clears the overlay when no error remains (`src/frontend/stmclient.cc:553`).
* **Port:** Does the same overlay work but also copies every send error into `impl->status` (`MoshCore::tick`, `win32/mosh_core.cc:410`), which `mosh_main` prints at session end (`win32/mosh_main.cc:147`).
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
* **Port:** Restores the console and emits `close_sequence()` without a final `next_frame()` (`win32/console_io.cc:721`).
* **Consequence:** The last rendered screen retains whatever overlay state — notifications, prediction underlines — was live when the loop exited.
* **Class:** `DEFECT`, low.

#### A21. Exit-time diagnostics and the exit banner are omitted

* **Upstream:** After restoring the terminal, prints either detailed initial-connection troubleshooting (firewall, UDP port range, `-p`) or a warning that `mosh-server` may still be running (`src/frontend/stmclient.cc:220`), and finally `[mosh is exiting.]` (`src/frontend/mosh-client.cc:215`).
* **Port:** `mosh_main` prints only `status_message()` when nonempty (`win32/mosh_main.cc:147`); there is no banner.
* **Consequence:** A failed initial connection gives no firewall or UDP guidance, an unclean exit gives no server-still-running warning, and a clean exit has no banner.
* **Class:** `DEFECT`, medium — the connection-failure guidance is the single most useful diagnostic upstream prints, and this is the platform where UDP is most likely to be firewalled.
* **Note:** split from A14 because the owners differ. A14 is a frame the session emits; this is text `main` prints after restoration.
* **Caution when implementing:** upstream's troubleshooting text recommends the `-p` option for selecting a UDP port. `mosh.exe` has no `-p` — it takes the port positionally (I7) — so the message must be rewritten for the standalone interface rather than copied.

---

## Confirmed parity (previously repaired)

#### The cached timestamp is frozen after the wait

* **Upstream:** `Select::select()` freezes the timestamp after `pselect` returns (`src/util/select.h:186`).
* **Port:** `run_loop()` calls `core.refresh_clock()` after `WaitForMultipleObjects` returns and before any dispatch (`win32/console_io.cc:905`); `MoshCore::refresh_clock` freezes the shared timestamp (`win32/mosh_core.cc:431`).
* **Status:** parity. Inbound state timestamps and RTT samples use wakeup time in both.

#### UCRT wide-printf semantics

* **Upstream:** Uses `%s` in wide `swprintf` calls for narrow strings (`src/frontend/stmclient.cc:199`).
* **Port:** Matching formats use `%s` (`win32/mosh_core.cc:80`), with the Windows build configured for POSIX wide-stdio semantics.
* **Status:** parity. The earlier UCRT interpretation could garble or read past narrow strings; this was a CRT conformance issue, not a state-machine difference.

## Seeds checked against the code

* `CONNECTION_TIMEOUT = 15000` is not an invented value. Upstream uses the same literal (`src/frontend/stmclient.cc:539`). The threshold matches; the transition does not — see A10.
* Timestamp placement is current parity, not a divergence.
* The resize seed's direction was backwards. It is the **port** that skips the prediction reset during shutdown, not upstream — see A8.
