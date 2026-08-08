# MoshCore / STMClient behavioral-parity inventory

`win32/mosh_core.cc` preserves the shared transport, overlay, prediction, and framebuffer algorithms, but it is not a literal extraction of `STMClient`: the host loop, console lifecycle, and several lifecycle and error paths differ. This inventory records those differences so each one is either justified or scheduled, rather than discovered later as a bug. The escape-key, prediction, overwrite, and title-prefix configuration parsers are shared with `STMClient` via `src/frontend/startup_config`, so both frontends parse those settings identically.

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
| `PLATFORM` | 8 |
| `POLICY` | 2 |
| `DEFERRED` | 0 |
| `DEFECT` | 37 |
| `OPEN` | 3 |
| **Total** | **50** |

Eighteen repaired or confirmed behavior records — connection-timeout shutdown, reader input ending, bare Ctrl-Z passthrough, interrupt control events versus a typed Ctrl-C, bounded close-handler restoration attempts, post-wait timestamp freezing, resize framebuffer ownership, UCRT wide-printf semantics, the retired command-line key channel, the bounded zero-length wait, the fresh-client-socket receive pass, `MOSH_NO_TERM_INIT`, `MOSH_ESCAPE_KEY`, `MOSH_PREDICTION_OVERWRITE`, the prediction display preference, the `[mosh] ` title prefix, transport verbosity, and astral-plane character decoding — are recorded at the end and are not counted as findings. The bounded zero-length wait is implemented rather than fully verified; the coverage it still owes is counted, as A26.

## How the defects cluster

Startup parsing is resolved for A1, A2, A3, and A25 through the shared `src/frontend/startup_config` module used by both frontends. A4's transport-verbosity tier is parsed independently by each frontend's getopt loop, and A20's presence check is independent in the Windows startup path; their behavior currently matches but can drift separately. The remaining configuration-adjacent defects are teardown behavior, not parsing: A13's escape-shutdown wording and A14's full normal-exit cleanup transition remain open, with A21 retaining the related exit diagnostics.

The event-loop, timing, and diagnostics cluster is **A5, A6, A7, A8, A9, A12, A15, A22, A23, A24, A26, A27, A29, A30, A31, and A32**. `STMClient::main()` was re-derived rather than extracted, so every ordering, exception-boundary, and poll-diagnostics decision was re-made independently; A27 is the unimplemented Select verbosity level-two-and-above tier of A4 and remains coupled to A26's wait seam. A29 and A30 are handle/event-registration lifetime defects; A31 is the Windows-only UDP reset error path; A32 is the status/cleanup reporting split. A42, A43, and A46 are the separate reader-thread and teardown-lifetime cluster. The final whole-branch review also found A33–A41 in bootstrap, locale, and console handling, and A47–A49 in the build and lifecycle bookkeeping; all A29–A49 records are for later scheduling, with fixes deferred.

A28 is an open behavior decision in the escape-suspend path: A16 covers only the genuinely platform-forced absence of SIGSTOP, while A28 covers the unresolved choice between an unsupported notification and literal pass-through, including whether the escape-prefix byte is forwarded.

Whether the correct remedy is a shared platform-neutral loop coordinator (taking normalized events, returning actions and deadlines) or platform-specific loops held together by parity tests is an open architectural question — a literal extraction of `STMClient::main()` is unlikely to stay simple, because the POSIX and Win32 waiting, input, resize, and termination contracts genuinely differ.

What must be settled first is the **behavioral contract**, not the code organization: the intended phase ordering, exception boundaries, timer semantics, fairness limits, and shutdown invariants, recorded as expected event traces rather than prose — ordinary input, simultaneous input and network readiness, receive error, send error, crypto error, resize during shutdown, a typed Ctrl-C reaching the remote, a first and a repeated interrupt control event, close or session-end termination, and a run of zero-length wait requests with the nonzero request that clears it. That last trace is there to keep an already-repaired invariant from being lost silently: a coordinator that reordered or dropped the throttle would still pass every other trace. It has to record the requested interval and the timeout the wait actually receives as separate values, and it constrains neither the iteration rate nor a wait whose handle is already signalled — what it forbids is an idle source that is repeatedly due issuing unbounded zero-length waits. It has to run again with an expired termination deadline, which must still drive the effective timeout to zero and take the restoration exit ahead of resize, socket, input, and frame dispatch, so that the floor can never delay close handling. Those traces serve either architecture and make the eventual coordinator decision evidence-based. Sharing an implementation stays an evaluated option, not a prerequisite. Recording a trace is not the same as being able to run one: the loop currently exposes no boundary a harness can enter, which is A26, and whichever architecture is chosen has to provide one.

Only the genuinely coupled findings wait on that contract: A5 and A9 (phase ordering) and A6, A7, and A15 (exception boundaries and retry timing). The cluster's former high-severity defect did not wait on it and is now repaired: the zero-wait throttle was an independent safety invariant with a local contract test, and holding a resource-exhaustion fix behind a speculative refactor would have been the wrong trade. The final whole-branch review added high-severity A29, A30, A31, A35, A42, A43, and A46; S2 remains a release blocker. A22 is local error-state bookkeeping and is likewise independently fixable.

The remaining defects are exit-path omissions (A13, A14, A21), the loop's missing test boundary and level-two diagnostics gap (A26, A27), the new event-loop/network, bootstrap, locale, console-lifecycle, and build defects (A29–A49), and one security defect (S2). A28 is an open suspend-behavior decision alongside these defects. The final whole-branch review found the A29–A49 records; they are inventory entries for later scheduling, not fixes made by this ledger update.

### The host loop, for reference

Several findings below depend on the exact phase order, so it is stated once here. `ConsoleSession::Impl::run_loop()` (`win32/console_io.cc:1157`) runs:

```
tick  →  wait  →  refresh clock
      →  termination check  →  reader-failure check  →  resize poll
      →  at most one readable socket  →  input  →  frame  →  (next iteration) tick
```

The wait's timeout is derived in three steps, in this order: the interval `tick()`
asks for, capped at `RESIZE_POLL_CAP_MS` (100 ms); then the poll throttle, which
raises a repeated zero to 1 ms; then the termination-deadline clamp, which may
lower it again, to zero.

There is no second wait between dispatch and the next `tick()`. Upstream's order is frame → wait → dispatch → tick, all in one pass.

---

## Findings

### Startup, configuration, and terminal model

#### I1. Native-console terminal lifecycle replaces termios and terminfo

* **Upstream:** `STMClient::init()` saves and raws `stdin` with termios and opens the terminal through the terminfo-configured `Display` (`src/frontend/stmclient.cc:82`); `Display(true)` consults terminfo including `smcup`/`rmcup` (`src/terminal/terminaldisplayinit.cc:83`).
* **Port:** Constructs `Display(false)` and delegates raw mode, UTF-8 code pages, and VT output to `ConsoleSession` (`win32/mosh_core.cc:112`, `win32/console_io.cc:905`).
* **Consequence:** The executable uses the Windows Console VT contract rather than the user's terminfo.
* **Class:** `PLATFORM`, low. Windows has no termios, and the native console requires `SetConsoleMode` plus VT escapes.


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
* **Class:** `POLICY`, medium. Out of scope by decision: the supported case is a host the client can route to directly, and no discovery work is scheduled. Recorded as a finding because it is a user-visible behavioral difference from upstream, not because it is planned. Revisit if a supported deployment lands behind NAT or a load balancer; nothing else should reopen it. The decision was re-examined on 2026-08-05 with a full scope of the upstream-parity fix — the `proxy` fake-proxy is upstream's only non-experimental discovery method, and matching it costs roughly 400–750 LOC dominated by a Windows stdio↔socket relay and unverified Windows OpenSSH `ProxyCommand` quoting — and deferred again on that basis. The revisit scope, risks, pin interactions, and test plan are recorded in the wsltty repo at `docs/superpowers/plans/2026-08-05-mosh-remote-ip-parity-deferral.md`; read it before re-deriving anything.
* **Fix, if the decision is revisited:** an override needs nothing secret on the command line — see the S1 record, which separates endpoint selection from key transport. Its shape is not settled here, and two values are involved that need not be equal: the port the server binds, which `mosh-server new -p PORT[:PORT2]` can already request (`src/frontend/mosh-server.cc:234`, forwarded by upstream as `--port`, `scripts/mosh.pl:159`) and this bootstrap does not expose, and the address and port the client aims at. An address rewrite alone covers only port-preserving translation. Implementing upstream's `local` or `proxy` discovery is the other path and keeps the interface unchanged.
* **Related:** A21 covers the missing exit diagnostics. This is where they would matter most: a timeout that named the address it tried would turn an unexplained hang into an answer. I10 covers the proxy pins, which are a separate restriction.

#### I10. Configured SSH routes are refused outright

* **Upstream:** honors the user's `ProxyJump` and `ProxyCommand` in its `remote` mode. Its other two discovery modes do not: `proxy` substitutes a `ProxyCommand` of its own (`scripts/mosh.pl:402`), and `local` rewrites the destination to a numeric address, which drops any `ssh_config` stanza matched on the host alias.
* **Port:** pins `ProxyJump=none` and `ProxyCommand=none` on every invocation (`win32/mosh_bootstrap.cc:172`), so a destination reachable only through a jump host fails at `ssh`.
* **Consequence:** sessions that would work are rejected. A bastion required for TCP/22 by policy does not imply an unroutable UDP address — the target's own address may be reachable over a VPN — but the bootstrap refuses the route before it learns the endpoint.
* **Class:** `OPEN`, medium. No rationale was recorded where the pins were introduced (`aa41fc8`), and the two this document previously offered do not hold: they do not stabilize the parsed stream, since those options select the transport beneath the SSH session rather than the remote's stdout channel, and they do not follow from I9's missing discovery, since a proxied route can announce a routable address. Either an invariant justifies failing closed and should be written down with its acceptance cases, or the pins should be lifted and the guarantee for a proxied route stated concretely — including whether an unroutable endpoint after a successful bootstrap is an accepted timeout, what it must print, and whether leaving the detached server to expire is accepted. Not `-S none`, which is a separate decision with a real reason; see S2.
* **Verification:** none. The command line is asserted exactly (`win32/test_bootstrap.cc:310`), which records the pins without testing what they exclude. What to test depends on which way this closes, and stating one matrix would presume the answer. Lift the pins, and the outcomes are that a bastion-routed SSH with a directly routable UDP endpoint completes a session and a jump-only private endpoint fails in bounded time naming the endpoint it tried — with `ProxyJump` and `ProxyCommand` covered separately, since their precedence differs and a bastion does not exercise an arbitrary proxy command. Keep them, and the outcome is that a configured route is refused with a diagnostic saying so rather than an opaque `ssh` failure, which is the case the current behavior does not satisfy either.
#### I8. The bootstrap forces no remote PTY, where upstream requests one

* **Upstream:** the wrapper defaults `$ssh_pty` to 1 (`scripts/mosh.pl:84`) and so passes `-tt` (`scripts/mosh.pl:359`), merging the server's stderr into the stream it parses (`scripts/mosh.pl:356`). `--no-ssh-pty` turns that off (`scripts/mosh.pl:167`).
* **Port:** passes `-T` unconditionally alongside `-n` (`win32/mosh_bootstrap.cc:164`), with no option to request a terminal. `-n` alone would not settle it: it redirects only the client's stdin, so a `RequestTTY=force` in the user's configuration would still allocate one.
* **Consequence:** `mosh-server` 1.2.4 and older exit when their initial `TIOCGWINSZ` fails and cannot be reached from this client; 1.2.5 falls back to 80x24 and works. That exclusion is not created by `-T`, and reading it that way overstates the option's cost: `ssh` allocates no terminal for a remote command unless one is requested, so the port never had a PTY to begin with. What `-T` removes is the single configuration that could reintroduce one, `RequestTTY force`, and with it a user's ability to reach a pre-1.2.5 server by that accident. `RequestTTY yes` is not in that set: it requests a terminal only when this side's standard input is one, and `-n` makes standard input the null device.
* **Consequence (user-visible, reproduced 2026-08-07):** the login motd is silently dropped. The motd is printed by `pam_motd.so` in sshd's PAM session stack (e.g. `/etc/pam.d/sshd`), not by the remote shell — so "run a login shell instead" cannot restore it, because the shell never sees it. pam_motd fires regardless of PTY, but its output is discarded without a terminal to write to; upstream's `-tt` is what makes it visible. A login shell would print whatever `~/.profile` prints, not the system motd. The PTY is ephemeral either way: it belongs to the ssh bootstrap, which exits as soon as `MOSH CONNECT` is parsed (`win32/mosh_bootstrap.cc:215`), and the long-lived session runs over UDP with no ssh and no bootstrap PTY. (mosh-server allocates its *own* PTY server-side for the user shell — a different PTY, present in both upstream and this port.)
* **Why upstream requests a PTY at all:** not for the motd, which is an accidental side-effect. `mosh-server` reads the initial window size with `TIOCGWINSZ` on stdin (`src/frontend/mosh-server.cc:423`); with a PTY it learns the real dimensions, without one it falls back to 80x24 — which the client overwrites on first connect anyway (`:425`). The PTY is otherwise a *liability* upstream works around: typeahead during the bootstrap window can echo into the parsed stream and corrupt `MOSH CONNECT`, so mosh-server prints a defensive `\r\n` when stdin is a TTY (`:443-449`). So `-tt` buys an immediately-overwritten window size and accidental motd visibility, at the cost of a typeahead-echo hazard; `-T` is the cleaner parse at the cost of the motd and pre-1.2.5 servers.
* **Class:** `OPEN`, medium — servers this client cannot reach at all is user-visible behavior, not a diagnostic difference, and the choice is genuinely unresolved rather than deliberately made. `POLICY` would claim the alternative is understood and unwanted; the rest of this entry says otherwise. The bootstrap parses a startup banner out of `ssh`'s piped stdout, and one stream shape it can rely on is a requirement of that design: the parse must not depend on settings the client does not control. Be exact about what is and is not claimed, though — no parse failure under a PTY has been reproduced. Upstream merges stderr into the parsed stream deliberately, this parser ignores lines that are not `MOSH ` records and strips a trailing carriage return, so a terminal would plausibly work. The case for `-T` is that a configuration nobody tests should not be able to change what the parser sees, and its cost is bounded to users who force a terminal. Supporting both shapes instead would mean testing both against every server the matrix admits. One deterministic shape does not uniquely select `-T`, either: a forced `-tt` is equally deterministic, is upstream's default, and would keep pre-1.2.5 servers reachable. It is not chosen here because `-T` is what the port already did — `ssh` allocates no terminal for a remote command unless asked — so `-T` pins the current behavior while `-tt` would change it, in a direction where merged stderr and line discipline reach the parser and nothing has been tested. Settling that properly means comparing both end to end over banners, remote stderr, CRLF, and the oldest admitted server. Until that comparison exists, `-T` is an interim posture — the conservative pin on current behavior — not a decided policy, which is why this is `OPEN`. Closing it means either running the matrix and preferring `-tt` if it passes, or establishing `mosh-server` 1.2.5 as a supported minimum for a reason that stands on its own. The motd reproduction above is the matrix's first concrete entry: it is a real user-visible cost of `-T`, and it is now known to be restorable only by `-tt`, not by any shell-side change.
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
* **Port:** Waits with `WaitForMultipleObjects` on the reader event, the termination event, and `WSAEventSelect(FD_READ)` events (`win32/console_io.cc:1209`); the console handler translates control events into the termination event (`console_control_handler`, `win32/console_io.cc:655`).
* **Consequence:** Control events and socket readiness are dispatched through handles rather than signals and fds.
* **Class:** `PLATFORM`, low.

#### A26. The event loop admits no test at its wait boundary

* **Upstream:** `Select` is a mockable seam in practice — its `select()` takes the timeout as an argument, so a caller can be driven with any sequence of intervals and its behavior observed at the call.
* **Port:** `run_loop()` computes the timeout from `core.tick()` and passes it to `WaitForMultipleObjects` inside the same function (`win32/console_io.cc:1201`, `win32/console_io.cc:1209`). Nothing external can supply the interval or observe the one the wait receives, and no harness can drive `MoshCore::tick()` to a chosen value: it returns the minimum of the transport's, the overlays', and the lifecycle's wait times (`win32/mosh_core.cc:425`).
* **Consequence:** every per-iteration decision the loop makes about time is unverifiable by test. That is why the throttle repaired above is covered by a unit test on its arithmetic and a CI check on the source's shape rather than by the executable check its finding originally called for, and the same limit will apply to any later timing rule. The acceptance harness can start the loop and observe what it leaves behind; it cannot observe what the loop does per iteration.
* **Class:** `DEFECT`, medium. Not `OPEN`: that a harness needs some way in is settled, and only where the boundary belongs is undecided.
* **Fix:** a production step the loop calls through, which takes the requested interval and the active deadline, owns the throttle state, computes the effective timeout, hands it to a wait adapter, and returns what the loop should do next — dispatch, or restore now. An observable wait alone is not enough, and neither is a helper a test can call directly: that only re-proves the arithmetic. The seam has to be able to supply what `MoshCore::tick()` returns, to be the only route to a wait, and to make the post-wait decision observable, or the wiring stays exactly as unverified as it is today. Its adapter must also preserve the existing error contract, so that `WAIT_FAILED` and an out-of-range result keep propagating rather than being read as a timeout. The rule itself stays fixed at upstream's values; it is an invariant to preserve, not a policy to make configurable. Not a test-only injector, which would compile into the release build for one assertion. Where the boundary belongs is what the behavioral contract above has to answer, so settle it there rather than ahead of it.
* **Acceptance:** the test must enter the path `run_loop()` itself uses, not call the timeout policy directly — otherwise it reproduces the unit test and leaves the wiring exactly as unverified as it is now. Script the requested intervals `0 ×9, 0, 0, nonzero, 0` and assert the timeout the wait adapter receives *exactly*: 0 for the first nine, exactly 1 ms for the tenth and the one after, the nonzero unchanged, and 0 again after it. Exact values matter — "at least 1 ms" would admit an implementation that substitutes 100 ms and breaks parity while passing. Cover the input domain too: below, at, and above the 100 ms resize cap, and `INT_MAX`. One case must return readiness immediately from a signalled handle while still having received exactly 1 ms, which is what distinguishes passing a timeout to the OS from sleeping before the wait. Then, with the floor engaged, an expired termination deadline must drive the effective timeout to zero and take the restoration-reserve exit ahead of resize, socket, input, and frame dispatch. Finally, assert `run_loop()` reaches no wait that bypasses the boundary; that is what retires the source-text check above.

#### I3. Reader thread, CESU-8 conversion, bounded queue, and fair dispatch replace `read(stdin)`

* **Upstream:** Reads up to 16 KiB synchronously from `STDIN_FILENO` once select reports it readable, then processes every returned byte in that pass (`STMClient::process_user_input`, `src/frontend/stmclient.cc:310`).
* **Port:** A worker blocks in `ReadFile`, recombines conhost CESU-8 surrogate pairs, and queues up to `INPUT_QUEUE_CAP_BYTES` (4 MiB); the owner consumes at most `INPUT_BUDGET_BYTES` (64 KiB) per wakeup (`Reader::read_loop`, `win32/console_io.cc:340`; `win32/console_io.cc:1290`).
* **Consequence:** A large paste is deliberately spread across loop iterations, so termination, resize, and socket work stay responsive; Windows input encoding is normalized before the shared parser sees it.
* **Class:** `PLATFORM`, low. Console `ReadFile` does not provide selectable byte-stream semantics, and an unbounded drain would starve the other handle sources.

#### I4. One readable socket is serviced per wakeup

* **Upstream:** Inspects every fd in the ready set but records only a boolean, invoking `process_network_input()` once per loop (`src/frontend/stmclient.cc:476`).
* **Port:** Re-tests the snapshot's WSA events, calls `core.on_readable()` for the first `FD_READ`, then breaks because the receive path can prune sockets (`win32/console_io.cc:1274`).
* **Consequence:** Both process one transport receive action per ordinary wakeup; the port additionally avoids dereferencing a snapshot invalidated by pruning.
* **Class:** `PLATFORM`, low.

#### A5. `tick()` runs before the wait, so frame generation sits between dispatch and transmission

* **Upstream:** Processes ready network, input, and resize work and then calls `network->tick()` in the same pass (`src/frontend/stmclient.cc:486`).
* **Port:** Calls `core.tick()` at the top of the iteration, before the wait (`win32/console_io.cc:1163`); dispatch happens after the wait, and the next `tick()` is reached only after `core.next_frame()` has been generated and written (`win32/console_io.cc:1295`).
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
* **Port:** Queries `GetConsoleScreenBufferInfo` after every wakeup — at most every 100 ms when idle, because of the `RESIZE_POLL_CAP_MS` cap — and calls `core.resize()` on a dimension change (`win32/console_io.cc:1258`).
* **Consequence:** Resize detection is delayed by up to the polling interval and costs a console round trip on idle wakeups.
* **Class:** `PLATFORM`, low. There is no SIGWINCH equivalent for the native console.

#### A8. During shutdown the port skips the prediction reset

* **Upstream:** `process_resize()` guards only the `push_back` of the resize instruction on `!shutdown_in_progress()`; it calls `overlays.get_prediction_engine().reset()` unconditionally (`src/frontend/stmclient.cc:418`).
* **Port:** `MoshCore::resize()` returns before both the enqueue and the reset when `finished` or `shutdown_in_progress()` (`win32/mosh_core.cc:378`).
* **Consequence:** A resize detected after graceful shutdown begins leaves stale local prediction overlays on screen until exit, where upstream invalidates them.
* **Class:** `DEFECT`, low.

#### A9. Initial and per-iteration frame timing differ

* **Upstream:** Writes a full empty frame before entering its first select (`src/frontend/stmclient.cc:256`), and writes a frame before computing readiness on every pass thereafter (`src/frontend/stmclient.cc:450`).
* **Port:** Writes the open sequence during setup (`win32/console_io.cc:951`) but calls `core.next_frame()` only after the wait and after dispatch (`win32/console_io.cc:1295`).
* **Consequence:** Initial screen initialization waits for the first wakeup — normally up to 100 ms — and there is no pre-wait frame emission.
* **Class:** `DEFECT`, low.

### Termination, errors, and exit

#### I6b. Only close is an interactive control-handler deadline

* **Upstream:** Has no equivalent; SIGHUP delivery imposes no kernel-enforced deadline on the handler's effects.
* **Port:** `console_control_handler` maps `CTRL_CLOSE_EVENT` to `ShutdownCause::CTRL_CLOSE`, publishes a deadline, and waits for the restoration attempt to complete (`win32/console_io.cc:655`, `win32/console_io.cc:663`, `win32/console_io.cc:676`, `win32/console_io.cc:681`).
* **Windows contract:** Microsoft's [HandlerRoutine documentation](https://learn.microsoft.com/windows/console/handlerroutine) gives `CTRL_CLOSE_EVENT` the `SPI_GETHUNGAPPTIMEOUT` budget (table value 5000 ms). It also states that `CTRL_LOGOFF_EVENT` and `CTRL_SHUTDOWN_EVENT` are received only by services: interactive applications are terminated before those signals are sent. Microsoft's [SetConsoleCtrlHandler documentation](https://learn.microsoft.com/windows/console/setconsolectrlhandler) adds that loading `gdi32.dll` or `user32.dll` suppresses those events and directs such applications to use a hidden window's `WM_QUERYENDSESSION` and `WM_ENDSESSION` messages. Thus an interactive `mosh.exe` can receive this control-handler deadline only for `CTRL_CLOSE_EVENT`.
* **Consequence:** Close must prioritize *local* console restoration over remote acknowledgement; an arbitrarily long protocol shutdown is not available.
* **Class:** `PLATFORM`, medium.
* **Design:** the handler signals the owner and blocks on a bounded completion event; see [The close control handler waits for the restoration attempt](#the-close-control-handler-waits-for-the-restoration-attempt) for why the wait is mandatory.

#### A23. Session end has no producer

* **Upstream:** SIGTERM and SIGHUP start network shutdown when a remote address exists (`src/frontend/stmclient.cc:508`).
* **Port:** Defines `ShutdownCause::SESSION_END` and a cross-thread `request_shutdown()` entry point (`win32/console_io.h:67`, `win32/console_io.cc:1117`), but no hidden window or message pump publishes that cause from `WM_QUERYENDSESSION` or `WM_ENDSESSION`.
* **Consequence:** On logoff, the client has no protocol-shutdown path and the remote `mosh-server` can be left running.
* **The producer cannot be scoped to `ConsoleSession`.** A session end can arrive during the SSH bootstrap — after `mosh-server` has started and returned its `MOSH CONNECT` reply, but before a `ConsoleSession` exists to receive `request_shutdown()`. In that window the notification has nothing to reach: the spawned `ssh` child is not reaped, the acquired key is not scrubbed, and the remote server is orphaned with no client that ever connected. Whatever owns the hidden window therefore has to outlive and precede the console session, and has to know which phase — bootstrapping, running, restoring — it is interrupting.
* **Correcting this is not simply wiring the two messages to `request_shutdown()`.** `WM_QUERYENDSESSION` is a query that may be refused or cancelled, and a cancelled session end is followed by `WM_ENDSESSION` with `wParam == FALSE`. Transport shutdown is irreversible, so publishing on the query would destroy a session the user just kept. Only a committed `WM_ENDSESSION` may publish, exactly once. The attainable guarantee is a bounded best-effort shutdown attempt before the callback returns, not a stopped remote server: Windows may terminate the process once the callback returns, and the message budget for session end is not the `SPI_GETHUNGAPPTIMEOUT` value that governs `CTRL_CLOSE_EVENT`.
* **Class:** `DEFECT`, medium. Windows supplies session-end notification through hidden-window messages; the missing producer is not platform-forced.

#### I6c. Console restoration is not reliable during close handling

* **Windows contract:** Microsoft's [HandlerRoutine documentation](https://learn.microsoft.com/windows/console/handlerroutine) states that console functions "may not work reliably" while processing `CTRL_CLOSE_EVENT`, `CTRL_LOGOFF_EVENT`, or `CTRL_SHUTDOWN_EVENT`, because console cleanup may already have run before the handler executes.
* **Port:** The close path attempts to restore input and output modes before it signals `restored` (`Impl::restore`, `win32/console_io.cc:1031`, `win32/console_io.cc:1046`; `Impl::release_and_signal`, `win32/console_io.cc:973`, `win32/console_io.cc:982`, `win32/console_io.cc:984`). The completion event means only that the attempt completed; the cleanup report records whether it succeeded (`win32/mosh_main.cc:173`).
* **Consequence:** Even when the handler waits within the close deadline, restoration can fail. The deadline machinery can bound the attempt; it cannot guarantee a restored console. Any user-visible cleanup report is best-effort because console output is itself among the operations documented as unreliable during close handling. Acceptance criteria for close must distinguish an attempt completing from restoration succeeding.
* **Class:** `PLATFORM`, medium.

#### A24. A blocked console write can consume the whole close deadline

* **Upstream:** No analogue. POSIX imposes no forced-termination deadline on the client, so a slow terminal write delays shutdown without cutting it short.
* **Port:** Every console write is a synchronous `WriteFile` on the owner thread — the open sequence during construction (`win32/console_io.cc:951`) and each frame in the loop (`win32/console_io.cc:1295`). Publishing a close deadline signals an event; it does not interrupt a write already in progress, and nothing cancels one.
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

#### A16. The escape-suspend suspension mechanism is unavailable

* **Upstream:** Ctrl-^ followed by Ctrl-Z closes the display, restores termios, prints `[mosh is suspended.]`, raises `SIGSTOP`, and calls `resume()` on continuation (`src/frontend/stmclient.cc:302`, `src/frontend/stmclient.cc:318`).
* **Port:** The escape-suspend branch has no Windows suspension mechanism (`win32/mosh_core.cc:252`).
* **Consequence:** Windows cannot suspend and resume the session through the upstream `SIGSTOP` mechanism.
* **Class:** `PLATFORM`, low. Windows has no `SIGSTOP`; the suspension mechanism itself is genuinely unavailable and not correctable. The port's separate choice to swallow the sequence silently is A28.

#### A28. Escape-suspend substitute behavior is unresolved

* **Upstream:** Always closes the display, restores termios, prints `[mosh is suspended.]`, calls `kill(0, SIGSTOP)`, and resumes on continuation (`src/frontend/stmclient.cc:302`, `src/frontend/stmclient.cc:311`, `src/frontend/stmclient.cc:316`, `src/frontend/stmclient.cc:318`). It has no no-`SIGSTOP` fallback branch.
* **Proposed Windows behaviors:** Report that suspension is unsupported, or pass the escape-prefix plus Ctrl-Z through as literal input. These are alternatives under consideration, not upstream behavior.
* **Port:** `feed_input` matches byte `0x1a` after the escape prefix and deliberately does nothing (`win32/mosh_core.cc:252`).
* **Consequence:** A user typing the escape-suspend sequence receives no notification and loses the input bytes, but the substitute behavior and whether the escape-prefix byte is forwarded remain undecided.
* **Class:** `OPEN`, low. A16 owns the platform limitation; choosing an unsupported notification versus literal pass-through has materially different remote-input semantics.
* **Related:** A16 owns the unavailable suspension mechanism; A28 owns the unresolved substitute behavior.

#### A14. Normal-exit cleanup is not one complete transition

* **Upstream:** On a normal exit, `STMClient::shutdown()` clears the notification, marks the server heard, clears the title prefix, and renders one final frame through the ordinary render path (`src/frontend/stmclient.cc:153`, `src/frontend/stmclient.cc:156`, `src/frontend/stmclient.cc:157`, `src/frontend/stmclient.cc:158`, `src/frontend/stmclient.cc:159`).
* **Port:** Restoration emits `close_sequence()` without a single MoshCore cleanup transition that performs the three explicit overlay mutations — notification clear, server-heard mark, and title-prefix clear — and then the final frame before restoration (`win32/console_io.cc:1031`, `win32/console_io.cc:1037`).
* **Consequence:** Normal exits can retain stale notifications and title state on the last rendered screen, and cleanup responsibilities are split across unrelated paths. The final frame follows the ordinary cull/apply render path and does not imply a separate prediction reset.
* **Class:** `DEFECT`, low. A14 owns the full normal-exit cleanup transition, including the final frame; it is not merely a missing frame.
* **Timing rule:** For deadline-driven or failed-output exits, the final frame is skipped in favor of immediate restoration. A synchronous final frame must not consume the close-handler restoration reserve; see A24.
* **Acceptance:** Cover local quit, peer-initiated shutdown, connection timeout, shutdown-ack timeout, fatal error, close-deadline termination, and output-write failure. The first six exercise the normal or immediate-restoration transition as applicable; output-write failure must verify restoration occurs without another frame attempt, and close-deadline termination must verify immediate restoration without a synchronous final frame.
* **Source note:** `STMClient::shutdown()` explicitly performs only the three listed overlay mutations and `output_new_frame()`; prediction invalidation is not a separate shutdown operation.

#### A21. Exit-time diagnostics and the exit banner are omitted

* **Upstream:** After restoring the terminal, prints either detailed initial-connection troubleshooting (firewall, UDP port range, `-p`) or a warning that `mosh-server` may still be running (`src/frontend/stmclient.cc:220`), and finally `[mosh is exiting.]` (`src/frontend/mosh-client.cc:215`).
* **Port:** `mosh_main` prints only `status_message()` when nonempty (`win32/mosh_main.cc:168`); there is no banner.
* **Consequence:** A failed initial connection gives no firewall or UDP guidance, an unclean exit gives no server-still-running warning, and a clean exit has no banner.
* **Class:** `DEFECT`, medium — the connection-failure guidance is the single most useful diagnostic upstream prints, and this is the platform where UDP is most likely to be firewalled.
* **Note:** split from A14 because the owners differ. A14 is the session's cleanup transition; this is text `main` prints after restoration.
* **Caution when implementing:** upstream's troubleshooting text recommends the `-p` option for selecting a UDP port. `mosh.exe` has no port selection of any kind — the endpoint comes from the server's `MOSH CONNECT` reply (I7) — so that sentence has no analogue and must be omitted rather than reworded. Do not reintroduce a positional port to give it one; whether client-side port selection should exist at all is a separate question this finding does not settle.

#### A27. Select poll diagnostics are unimplemented above transport verbosity

* **Upstream:** Repeatable `-vv` (verbosity greater than one) enables `Select` diagnostics, emitting per-poll and rate-limiting/throttle diagnostics (`src/util/select.h:128`, `src/util/select.h:132`, `src/util/select.h:137`). A single `-v` emits no Select diagnostics.
* **Port:** The Win32 `PollThrottle` enforces the wait floor but emits no poll or throttle diagnostics (`win32/console_io.cc:757`); `-vv` reaches transport only.
* **Consequence:** `-vv` and higher cannot expose the poll timing and throttling information available upstream.
* **Class:** `DEFECT`, low.
* **Related:** A4 is the confirmed transport-verbosity tier; A27 is its unimplemented Select tier for verbosity level two and above and is coupled to A26's missing wait seam.

### Final whole-branch review additions

#### A29. Enumerating a readable socket can strand a later socket's FD_READ event

* **Upstream:** `Select::select()` returns the complete ready set, and `process_network_input()` performs one receive action for the pass (`src/frontend/stmclient.cc:476`, `src/frontend/stmclient.cc:296`).
* **Port:** `run_loop()` enumerates each signaled socket with `WSAEnumNetworkEvents`, which clears the recorded network event, then calls `core.on_readable()` for the first `FD_READ` and breaks (`win32/console_io.cc:1274`, `win32/console_io.cc:1277`, `win32/console_io.cc:1283`). `on_readable()` discards the socket argument and sweeps the transport sockets in deque order (`win32/mosh_core.cc:362`, `src/network/network.cc:489`).
* **Consequence:** If the first socket consumes the 32-datagram budget (`win32/mosh_core.cc:66`, `win32/mosh_core.cc:201`) or receives a non-`No packet received` exception (`win32/mosh_core.cc:205`), `Connection::recv()` returns or throws after the first productive socket (`src/network/network.cc:513`, `src/network/network.cc:517`). A later socket whose `FD_READ` record was already cleared by enumeration is then neither received from nor re-armed, so its traffic can remain frozen until another event re-arms it.
* **Class:** `DEFECT`, **high** — event-loop/network. A readable event can be consumed as bookkeeping without a corresponding `recvfrom`, and a port hop can leave the user waiting indefinitely.
* **Fix:** Preserve the ready socket identity through `on_readable()` and receive from that socket, or re-arm every enumerated socket whose record was cleared before continuing.

#### A30. Socket-event registrations are keyed only by recycled `SOCKET` values

* **Upstream:** File descriptors in the selected set identify the live descriptor for that poll; a closed descriptor is not silently reused as the same registration.
* **Port:** `SocketEvents::reconcile()` stores registrations in `std::map<intptr_t, WSAEVENT>` and compares only the raw socket value (`win32/console_io.cc:601`, `win32/console_io.cc:620`, `win32/console_io.cc:630`). It does not re-issue `WSAEventSelect` when a value remains present (`win32/console_io.cc:630-633`).
* **Consequence:** Across iterations, `run_loop()` calls `core.tick()` before reconciliation (`win32/console_io.cc:1163`, `win32/console_io.cc:1168`). A receive-side prune can close a socket after the existing registration was captured, and a newly created socket can receive the same handle value before the next reconciliation. The map then treats the new socket as the old one and leaves it without `WSAEventSelect(FD_READ)`, so it is never reported readable.
* **Class:** `DEFECT`, **high** — event-loop/network. Raw handle identity is not a socket generation identity.
* **Fix:** Re-issue `WSAEventSelect` for every live socket on reconciliation, or track socket generations rather than only the raw value.

#### A31. Unconnected UDP does not tolerate `WSAECONNRESET`

* **Upstream:** The POSIX receive loop continues over transient nonblocking receive conditions and does not expose Windows' asynchronous ICMP reset as a receive failure (`src/network/network.cc:505-509`).
* **Port:** The Windows continuation list accepts only `WSAEWOULDBLOCK` and `WSAEMSGSIZE`; every other `recvfrom` error is thrown (`src/network/network.cc:489`, `src/network/network.cc:497-503`). The Windows socket setup does not configure `SIO_UDP_CONNRESET`, and the Windows config leaves the relevant optional socket features undefined (`win32/config.h.clangarm64:88-89`).
* **Consequence:** After an ICMP port-unreachable response, an unconnected UDP socket can report `WSAECONNRESET` on `recvfrom`. The port treats that platform-specific condition as a fatal receive error, abandons the read cycle, and can degrade roaming instead of continuing as POSIX does.
* **Class:** `DEFECT`, **high** — Windows-only network error handling.
* **Fix:** Decide and implement the Windows UDP reset policy, either disabling the reset notification for these sockets or treating the documented reset condition as a nonfatal receive result.

#### A32. Remote logout does not set the user-quit status

* **Upstream:** The exit path distinguishes user-requested shutdown from remote/session termination when selecting its final status and diagnostics (`src/frontend/stmclient.cc:344`, `src/frontend/stmclient.cc:508`).
* **Port:** `begin_shutdown()` sets `status` to `"Exiting..."` for the local shutdown path (`win32/mosh_core.cc:295`, `win32/mosh_core.cc:306`), while `mosh_main` prints whatever status the session leaves behind (`win32/mosh_main.cc:180-184`). Remote logout can finish through the lifecycle paths without assigning that status.
* **Consequence:** A user quit reports `Exiting...`, but a remotely initiated logout does not receive the same status transition or a corresponding status message.
* **Class:** `DEFECT`, low — exit diagnostics.

#### A33. Non-`ConsoleError` setup throws lose the cleanup report

* **Upstream:** The frontend's outer exception handling preserves the terminal cleanup and reports the resulting failure after teardown (`src/frontend/mosh-client.cc:203-215`).
* **Port:** `run_console_session()` assigns `*cleanup` only after `ConsoleSession` construction succeeds or inside the catch surrounding `session.run()` (`win32/mosh_main.cc:113-127`). Its outer `ConsoleError`, `NetworkException`, and `std::exception` handlers only scrub the key and set the message/status (`win32/mosh_main.cc:129-135`).
* **Consequence:** A non-`ConsoleError` throw during session construction or setup can execute the console rollback path but return without its `CleanupReport`; `mosh_main` then cannot print the rollback failure or reader error (`win32/mosh_main.cc:182-190`).
* **Class:** `DEFECT`, medium — cleanup diagnostics.

#### A34. `SearchPathW` receives raw `PATH` components

* **Upstream:** Resolves the SSH executable through the process environment's path semantics (`scripts/mosh.pl:78`, `scripts/mosh.pl:409`).
* **Port:** Reads `PATH` and passes the unnormalized string directly as `SearchPathW`'s `lpPath` (`win32/mosh_bootstrap.cc:230-236`, `win32/mosh_bootstrap.cc:246-253`). Relative components therefore resolve relative to the process's current directory under the Windows search-path contract, rather than to a fixed path-list base. The source does not establish the exact behavior of empty components, so this record does not claim one.
* **Consequence:** A caller-controlled relative `PATH` entry can resolve `ssh.exe` relative to a changing working directory, making executable selection depend on CWD and weakening the intended explicit-path lookup boundary.
* **Class:** `DEFECT`, medium — executable resolution.
* **Fix:** Normalize or reject relative path components before passing the list to `SearchPathW`, and define the intended empty-component behavior.

#### A35. Bootstrap draining can block before its timeout starts

* **Upstream:** The wrapper's SSH child and startup parsing are coordinated so a child that stops producing output can still be terminated by the surrounding lifecycle (`scripts/mosh.pl:409`).
* **Port:** `spawn_and_drain()` calls the blocking `drain_and_parse()` before it waits for the child (`win32/mosh_bootstrap.cc:350-354`). `drain_and_parse()` performs an unbounded blocking `ReadFile` loop (`win32/mosh_bootstrap.cc:192-218`), so the ten-second wait and terminate path are reached only after the pipe closes or the parser returns. The test covers a fixture that emits `CONNECT` and then sleeps, exercising only the post-drain reap (`win32/test_bootstrap.cc:436-441`).
* **Consequence:** An SSH child that keeps its stdout pipe open without producing a complete reply can hang the bootstrap forever; the documented ten-second process wait cannot bound that case.
* **Class:** `DEFECT`, **high** — bootstrap liveness.
* **Fix:** Make output draining and child-liveness supervision concurrent, or use a cancellable/overlapped pipe read whose deadline covers the drain itself.

#### A36. Forced SSH termination is reported as the child's status

* **Upstream:** The wrapper's parent reads the SSH pipe as a line stream and reports its own connection/bootstrap failure rather than formatting a separately reaped child status (`scripts/mosh.pl:412-425`).
* **Port:** After the ten-second wait, `spawn_and_drain()` calls `TerminateProcess(..., 1)` and then treats any exit code other than `STILL_ACTIVE` as a real child exit (`win32/mosh_bootstrap.cc:353-358`). It later formats every nonzero code as `ssh exited with status` (`win32/mosh_bootstrap.cc:363-368`).
* **Consequence:** A forced termination is surfaced as SSH status 1 instead of identifying the bootstrap timeout. In addition, the valid Windows process exit code 259 is indistinguishable from `STILL_ACTIVE` in the `have_exit` test.
* **Class:** `DEFECT`, medium — bootstrap diagnostics.

#### A37. CRT bootstrap banners can overtake raw console session output

* **Upstream:** Startup diagnostics and session output share the frontend's ordered output path (`src/frontend/mosh-client.cc:215`, `src/frontend/stmclient.cc:256`).
* **Port:** `drain_and_parse()` prints non-`MOSH` banners through CRT `stdout` (`win32/mosh_bootstrap.cc:213-217`), while the session writes frames and the open sequence directly with `WriteFile` (`win32/console_io.cc:740-755`, `win32/console_io.cc:946-951`). No flush bridges those two output paths before the session begins.
* **Consequence:** With redirected or buffered CRT stdout, a banner/MOTD can remain buffered and appear after raw session output rather than in bootstrap order.
* **Class:** `DEFECT`, low — output ordering.
* **Fix:** Use one output path or flush CRT stdout before handing the console output to the session.

#### A38. The optional Windows MTU-discovery failure path leaks its socket

* **Upstream:** A socket-construction failure closes the descriptor before propagating the error (`src/network/network.cc:201-206`).
* **Port:** The Windows constructor closes the socket for a nonblocking-mode failure but not when `setsockopt(IP_MTU_DISCOVER)` fails (`src/network/network.cc:155-175`). The ARM64 configuration currently leaves `HAVE_IP_MTU_DISCOVER` undefined (`win32/config.h.clangarm64:88`), so this is latent in the current build.
* **Consequence:** A build or configuration enabling the feature leaks every socket whose MTU-discovery setup fails.
* **Class:** `DEFECT`, low — latent resource leak.

#### A39. The optional Windows ECN receive path contradicts its own policy and uses POSIX diagnostics

* **Upstream:** Requests and consumes the ECN receive metadata when the platform supports it (`src/network/network.cc:215-223`).
* **Port:** The Windows comment says ECN is deliberately not requested because `IP_RECVTOS`/`recvmsg` is unavailable, but an optional `HAVE_IP_RECVTOS` block still calls `setsockopt` and reports failure with POSIX `perror` (`src/network/network.cc:178-191`). The feature is currently undefined in the ARM64 configuration (`win32/config.h.clangarm64:89`).
* **Consequence:** Enabling the feature would contradict the stated Not-ECT policy and could emit a misleading errno-based diagnostic for a WinSock error; the current build hides the defect rather than resolving it.
* **Class:** `DEFECT`, low — latent network diagnostics/policy drift.

#### A40. Windows locale detection tests the ANSI code page, not the active UTF-8 locale

* **Upstream:** `locale_charset()` reports the codeset selected by the active locale (`src/util/locale_utils.cc:73-93`).
* **Port:** The Windows branch reads `LOCALE_IDEFAULTANSICODEPAGE` from `LOCALE_USER_DEFAULT` and returns `UTF-8` only when that user-locale property is 65001 (`src/util/locale_utils.cc:78-84`). The startup explicitly selects `.UTF-8`, while the Windows core test has to work around the mismatch by asserting the locale and conversion directly (`win32/mosh_main.cc:147`, `win32/test_core.cc:91-98`).
* **Consequence:** A `.UTF-8` process locale under a non-65001 user ANSI code page is reported as `US-ASCII`, so `is_utf8_locale()` is false even though the active CRT locale is UTF-8.
* **Class:** `DEFECT`, low — locale detection.
* **Fix:** Query the active CRT locale's codeset or make the process's explicit UTF-8 locale state the source of truth.

#### A41. Clearing locale variables updates the Win32 environment but not CRT `_environ`

* **Upstream:** `clear_locale_variables()` uses `unsetenv`, updating the process environment visible to the C runtime (`src/util/locale_utils.cc:123-145`).
* **Port:** The Windows branch calls `SetEnvironmentVariableA(name, NULL)` for each variable (`src/util/locale_utils.cc:123-132`). That Win32 API does not synchronize the CRT's `_environ` table, so later CRT environment reads can retain the removed values. The Windows `mosh-server` path is not currently built, making this latent.
* **Consequence:** A Windows code path that clears locale variables and then consults the CRT environment can continue to observe stale locale settings.
* **Class:** `DEFECT`, low — latent locale handling.

#### A42. `WAIT_FAILED` is treated as a worker exit

* **Upstream:** A failed wait is an error, not evidence that the worker terminated (`src/util/select.h:143`).
* **Port:** `Reader::stop_until_deadline()` breaks on every result other than `WAIT_TIMEOUT`, including `WAIT_FAILED`, then closes the worker handle and nulls the member (`win32/console_io.cc:548-589`).
* **Consequence:** If the worker wait fails, the owner can report teardown complete and close the handle while the reader thread is still running, creating a use-after-close and leaving the input lifecycle unbounded.
* **Class:** `DEFECT`, **high** — thread lifecycle.
* **Fix:** Distinguish `WAIT_OBJECT_0`, `WAIT_TIMEOUT`, and `WAIT_FAILED`; retain the handle and surface the failure unless termination policy explicitly and safely cancels the worker.

#### A43. Reader teardown closes a duplicated input handle during an in-flight read

* **Upstream:** Synchronous input ownership ends after the read operation returns (`src/frontend/stmclient.cc:310-316`).
* **Port:** Teardown cancels synchronous I/O, exchanges `input` to null, and closes the duplicate (`win32/console_io.cc:536-544`, `win32/console_io.cc:574-578`), while the worker separately loads `input` and calls `ReadFile` (`win32/console_io.cc:370-377`).
* **Consequence:** The worker can load the handle between `input.load()` and `ReadFile` while teardown closes it, so cancellation does not establish ownership of the handle for the in-flight call. The read can fail against a recycled handle or observe invalid state during shutdown.
* **Class:** `DEFECT`, **high** — thread/handle lifetime.
* **Fix:** Give the worker stable handle ownership until it exits, then close it after a successful join; cancellation must not close a handle still usable by the worker.

#### A44. Restoring the original input mode can leave QuickEdit disabled

* **Upstream:** Restores the terminal's input mode through the same terminal-state contract used before raw mode (`src/frontend/stmclient.cc:153`).
* **Port:** Setup enables `ENABLE_EXTENDED_FLAGS` while clearing `ENABLE_QUICK_EDIT_MODE` (`win32/console_io.cc:907-909`), but restore passes the captured mode without ensuring `ENABLE_EXTENDED_FLAGS` is present (`win32/console_io.cc:1046-1050`).
* **Consequence:** When the original mode had QuickEdit enabled without the extended-flags bit, Windows ignores the QuickEdit setting during restoration, leaving QuickEdit disabled after exit.
* **Class:** `DEFECT`, low — console restoration.
* **Fix:** Include `ENABLE_EXTENDED_FLAGS` when restoring a mode whose QuickEdit bit must be honored.

#### A45. Restore failures can be silently omitted from the cleanup report

* **Upstream:** Cleanup diagnostics preserve a failed restoration operation rather than relying on an unrelated prior error state (`src/frontend/stmclient.cc:153-159`).
* **Port:** Code-page and mode restoration calls report failure through `record_last_error()` (`win32/console_io.cc:1040-1051`), which records `GetLastError()` only when `first_error` is still `ERROR_SUCCESS` (`win32/console_io.cc:170-179`).
* **Consequence:** If a failed Win32 call leaves `GetLastError()` as `ERROR_SUCCESS`, the report remains apparently clean and `mosh_main` suppresses the rollback warning (`win32/mosh_main.cc:185-190`). These calls have no generic failure backstop unlike the close-sequence write.
* **Class:** `DEFECT`, low — cleanup diagnostics.
* **Fix:** Record a generic restore failure whenever the boolean operation fails and `GetLastError()` is `ERROR_SUCCESS`.

#### A46. Exception unwinding can perform an unbounded reader join after a deadline

* **Upstream:** The main loop's exit path does not leave a detached worker whose destructor can block indefinitely (`src/frontend/stmclient.cc:490-572`).
* **Port:** The `Reader` comment explicitly says that a deadline teardown can leave `worker` non-null and requires callers to release such sessions rather than unwind (`win32/console_io.cc:591-598`). However, `run_console_session()` stores `ConsoleSession` by value and lets exceptions escape its inner catch (`win32/mosh_main.cc:117-125`).
* **Consequence:** If a deadline path cancels without joining the reader and `session.run()` then throws, stack unwinding destroys `ConsoleSession`; `Reader::~Reader()` calls the unbounded `stop()` path, which waits indefinitely on the still-running worker (`win32/console_io.cc:473-476`, `win32/console_io.cc:548-599`).
* **Class:** `DEFECT`, **high** — termination/thread lifecycle.
* **Fix:** Make the unwind path release the deliberately non-joined worker without blocking, or ensure every exception path retains a bounded teardown contract.

#### A47. Protobuf header prerequisites hardcode the default build directory

* **Upstream:** Generated-header prerequisites follow the selected build directory rather than embedding a platform-specific default (`src/protobufs/Makefile.am:1-8`).
* **Port:** `BUILD` is configurable (`win32/Makefile.win:48`), but the network and state-sync prerequisite rules hardcode `win32/build/...` (`win32/Makefile.win:147-155`).
* **Consequence:** Overriding `BUILD` leaves those object rules with stale prerequisite paths, so parallel builds can compile against missing or concurrently generated protobuf headers and reopen the race the dependency rules are meant to close.
* **Class:** `DEFECT`, medium — build correctness.
* **Fix:** Express each target with `$(BUILD)/...`.

#### A48. The protobuf archive is always rebuilt because `.PHONY` is a prerequisite

* **Upstream:** A library target is rebuilt when its object prerequisites are newer, not because a phony aggregate is named as a library prerequisite (`src/protobufs/Makefile.am:1-8`).
* **Port:** `protobufs` is phony (`win32/Makefile.win:72`), and `src/protobufs/libmoshprotos.a` lists it as a prerequisite (`win32/Makefile.win:114-115`).
* **Consequence:** Every make invocation considers the protobuf archive stale and relinks it, causing unnecessary rebuilds of all dependents and obscuring genuine dependency changes.
* **Class:** `DEFECT`, low — build hygiene.
* **Fix:** Keep the generated-header aggregate as an order-only or object-generation dependency without making the phony target a normal archive prerequisite.

#### A49. `ConsoleLifecycleState` is bookkeeping that never reaches `DONE`

* **Upstream:** Lifecycle state is consumed by the owning loop or teardown path rather than being written as unused bookkeeping (`src/frontend/stmclient.cc:153-159`).
* **Port:** `ConsoleLifecycleState` declares `DONE` but `Impl::state` is initialized to `RUNNING`, assigned `SHUTTING_DOWN` and `RESTORED`, and never read or assigned `DONE` (`win32/console_io.h:70-78`, `win32/console_io.cc:815-823`, `win32/console_io.cc:983`, `win32/console_io.cc:1143`).
* **Consequence:** The lifecycle state cannot express teardown completion and provides no invariant or diagnostic value; the three writes are dead bookkeeping that can drift from the actual release state.
* **Class:** `DEFECT`, low — lifecycle bookkeeping.

---

## Confirmed parity

#### A20. `MOSH_NO_TERM_INIT` gates only the alternate-screen pair

* **Upstream:** `Display::open()`/`close()` emit `smcup`/`rmcup` only when terminfo initialization was not suppressed; `MOSH_NO_TERM_INIT` suppresses it (`src/terminal/terminaldisplayinit.cc:83`).
* **Port:** Reads `MOSH_NO_TERM_INIT` during startup parsing (`win32/mosh_main.cc:167`, `win32/startup_options.cc:53`) and gates the literal alternate-screen pair around `display.open()`/`display.close()` (`win32/mosh_core.cc:146`).
* **Consequence:** Setting `MOSH_NO_TERM_INIT` keeps the session on the primary screen buffer, matching upstream. Application-cursor mode is unaffected; only the alternate-screen pair is gated.
* **Status:** parity. The Windows startup path gates the same alternate-screen pair; application-cursor mode remains unaffected.
* **Verification:** `win32/test_startup_options.cc` covers absent/set `MOSH_NO_TERM_INIT` in `test_no_term_init()`; `test_console_lifecycle.exe no-term-init` covers the emitted alternate-screen behavior.

#### A1. `MOSH_ESCAPE_KEY` is parsed identically by both frontends

* **Upstream:** Parses `MOSH_ESCAPE_KEY`, accepts one ASCII key or disables the parser on an empty value, derives the literal-pass spelling and line-start rule, and rejects dangerous controls (`src/frontend/startup_config.cc:42`).
* **Port:** `parse_startup_options` uses the shared `parse_escape_key` result, and `MoshCore` consumes the parsed key, literal-pass keys, line-start rule, and help spelling (`win32/startup_options.cc:51`, `win32/mosh_core.cc:116`, `win32/mosh_core.cc:128`).
* **Consequence:** Windows accepts the same escape-key settings and disables the parser for an empty value, with the same literal-pass and line-start behavior as `STMClient`.
* **Status:** parity. The parser is shared with `STMClient` via `src/frontend/startup_config`, so both frontends parse identically.
* **Verification:** `win32/test_startup_options.cc::test_escape_key()` covers the parsed snapshot; `test_console_lifecycle.exe escape-key` covers custom, non-escape, and disabled parser behavior; default Ctrl-^ quit is covered by `win32/test_console_lifecycle.cc:1075` and `:1157`.

#### A2. `MOSH_PREDICTION_OVERWRITE=yes` is parsed identically by both frontends

* **Upstream:** Enables insertion-overwrite prediction when the variable is exactly `yes` (`src/frontend/startup_config.cc:125`, used by `src/frontend/stmclient.h:114`).
* **Port:** `parse_startup_options` parses the same value and `MoshCore` enables `PredictionEngine::set_predict_overwrite()` when it is true (`win32/startup_options.cc:50`, `win32/mosh_core.cc:123`).
* **Consequence:** Insert and delete prediction use the same overwrite setting before the server echo arrives.
* **Status:** parity. The parser is shared with `STMClient` via `src/frontend/startup_config`, so both frontends parse identically.
* **Verification:** `win32/test_startup_options.cc::test_prediction_overwrite()` covers the `yes` snapshot; the startup-options test also covers the shared parser wiring used by both frontends.

#### A25. The prediction display preference is parsed identically by both frontends

* **Upstream:** `STMClient` and the Windows startup path use the shared `parse_prediction_display` parser, accepting `always`, `never`, `adaptive`, and `experimental`, with an absent variable selecting `Adaptive` (`src/frontend/startup_config.cc:101`).
* **Port:** Reads and validates `MOSH_PREDICTION_DISPLAY` before `mosh_bootstrap`, then passes the typed preference to `MoshCore` (`win32/mosh_main.cc:163`, `win32/startup_options.cc:45`, `win32/mosh_core.cc:123`).
* **Consequence:** Windows exposes the same four prediction display preferences and rejects invalid values before starting the remote server.
* **Status:** parity. The parser is shared with `STMClient` via `src/frontend/startup_config`, so both frontends parse identically.
* **Verification:** `win32/test_startup_options.cc::test_prediction_display()` covers absent, accepted, and invalid preference snapshots.

#### A3. The `[mosh] ` title prefix is applied like `STMClient`

* **Upstream:** `STMClient::init()` sets the prefix unless `MOSH_TITLE_NOPREFIX` is set, and `STMClient::shutdown()` clears it before the final frame (`src/frontend/stmclient.cc:127`, `src/frontend/stmclient.cc:158`).
* **Port:** The shared parser determines whether the prefix is wanted, `MoshCore` applies `[mosh] ` during construction, and `begin_shutdown()` clears it on a local quit (`win32/startup_options.cc:52`, `win32/mosh_core.cc:154`, `win32/mosh_core.cc:308`).
* **Consequence:** Remote title updates are marked as mosh titles during the session and the prefix is cleared on a local quit. On a peer-initiated exit, the title can remain stale until A14's complete normal-exit cleanup transition is implemented; this is an accepted deferred coupling between A3 and A14.
* **Status:** parity within the implemented lifecycle. The parser is shared with `STMClient` via `src/frontend/startup_config`, so both frontends parse identically; peer-initiated final cleanup remains deferred under A14.
* **Verification:** `win32/test_startup_options.cc::test_title_prefix()` covers absent/set `MOSH_TITLE_NOPREFIX`; the A14 acceptance matrix must cover peer-initiated teardown timing.

#### A4. Repeatable `-v` enables transport diagnostics

* **Upstream:** Parses repeatable `-v` and calls `network->set_verbose()` (`src/frontend/mosh-client.cc:129`, `src/frontend/stmclient.cc:220`).
* **Port:** `parse_invocation` uses `getopt` to count repeatable `-v` options, and `MoshCore` applies the count to the transport (`win32/mosh_bootstrap.cc:377`, `win32/mosh_bootstrap.cc:384`, `win32/mosh_core.cc:142`).
* **Consequence:** Windows can request transport verbosity with `-v`; poll diagnostics remain outside this finding.
* **Status:** parity for transport verbosity only. The option parser uses `getopt`; `Select` poll diagnostics are not implemented. The unimplemented poll-diagnostics tier is tracked separately as A27.
* **Verification:** `win32/test_bootstrap.cc::test_parse_invocation()` covers repeatable `-v`/`-vv`; the CI “mosh.exe rejects bad invocations” usage-code check covers invalid option forms and the supported invocation contract.

#### Reader input ending enters graceful shutdown

* **Upstream:** `process_user_input()` returns false for EOF and a read error (`src/frontend/stmclient.cc:316`); the main loop then breaks if not yet connected, or calls `network->start_shutdown()` if connected (`src/frontend/stmclient.cc:490`).
* **Port:** `Reader::read_loop` marks a `ReadFile` failure as input ended (`win32/console_io.cc:370`); the owner observes `has_ended()` and begins graceful shutdown with `ShutdownCause::IO_LOSS`. A successful zero-byte read is *not* end-of-input when the handle is a console: see the Ctrl-Z record below.
* **Status:** read-failure parity. A `ReadFile` failure ends input. A successful zero-byte read is handled separately (below), because on a console handle it is how an old console host reports Ctrl-Z, not EOF.

#### Bare Ctrl-Z passes through to the server

* **Upstream:** a bare Ctrl-Z is an ordinary byte; the raw-mode client `read()`s it and forwards it to the remote pty, where it suspends the remote foreground job. Only escape-prefix Ctrl-Z is the local suspend (`src/frontend/stmclient.cc:302`), which Windows cannot do (A16/A28).
* **Port:** on a console host older than [microsoft/terminal PR #19940](https://github.com/microsoft/terminal/pull/19940), a raw-mode `ReadFile` reports Ctrl-Z as a *successful zero-byte read* and consumes the 0x1A byte. `Reader::read_loop` reinterprets a zero-byte read on a classified console handle (`GetFileType == FILE_TYPE_CHAR` and `GetConsoleMode` succeeds — the win32 OpenSSH pair) as Ctrl-Z and synthesizes the 0x1A into the input queue with the same cancellable backpressure as ordinary input, so the byte reaches the server exactly as upstream's does. A fixed host (PR #19940, which honors Ctrl-Z only in PROCESSED mode) delivers 0x1A as an ordinary byte and never reaches the synthesis branch, so old and new hosts behave identically. The read is per-keystroke, not latched — the next `ReadFile` blocks — so synthesis does not spin. Non-console input still ends the session on a genuine EOF: a closed pipe fails the `ReadFile` (`ERROR_BROKEN_PIPE`), and a regular file at EOF returns a zero-byte success — both take the non-console zero-byte/failure path, since only a console handle is eligible for the Ctrl-Z reinterpretation.
* **Why not `ReadConsoleInputW`:** the low-level event API identifies Ctrl-Z unambiguously (a `KEY_EVENT` with `uChar == 0x1A`), avoiding the zero-byte inference entirely. It was rejected because it drops the port a layer below upstream's architecture: upstream mosh is a byte-stream client whose terminal/pty does key-to-byte translation, and the high-level `ReadFile` path is the Windows analog (conhost plays the pty's role). `ReadConsoleInputW` would make the fork own dead-key composition, IME, and AltGr handling that upstream deliberately does not — a larger, permanent divergence to fix one documented quirk. The zero-byte inference is bounded: for a synchronous nonzero-buffer console read, the conhost source produces a zero-byte success only for Ctrl-Z — the conversion lives in [`ApiDispatchers.cpp`](https://github.com/microsoft/terminal/blob/main/src/server/ApiDispatchers.cpp#L313-L333) (a leading 0x1A sets `NumBytes` to zero after the record is consumed), and no other path in the read produces a successful zero-byte result — and the handle classification confines the rule to console handles. This rests on a component the port does not control; the probe modes below exist to catch a host that breaks the assumption.
* **Consequence:** bare Ctrl-Z suspends the remote job on every Windows console host, matching upstream and WSL. The earlier behavior — treating the zero-byte read as end-of-input, which killed the session — was the bug this record replaces.
* **Verification:** `test_console_lifecycle.exe ctrl-z-passthrough` simulates the old host via the `ZERO_BYTE_READ` injector and asserts the server receives the 0x1A `UserByte` (this is the deterministic acceptance coverage of the synthesis branch); `probe-ctrlz-e2e` runs the same assertion against a physically injected Ctrl-Z on the real host; `probe-ctrlz-read` is non-asserting diagnostic evidence that records the attached host's raw `ReadFile` response. The CI `windows-11-arm` conhost is a fixed host (it delivers literal 0x1A), so the synthesis branch is exercised only through the injector on CI; the physical-KeyEvent path on a genuinely old host is verified manually.

#### Connection timeout enters graceful shutdown

* **Upstream:** After more than 15,000 ms without a remote state, sets the notification and calls `network->start_shutdown()` (`src/frontend/stmclient.cc:539`); the transport then bounds shutdown by 16 packets or 10 seconds (`src/network/transportsender-impl.h:373`).
* **Port:** Uses the same `CONNECTION_TIMEOUT = 15000` (`win32/mosh_core.cc:64`) and calls `network->start_shutdown()` after setting the notification (`MoshCore::Impl::update_lifecycle`, `win32/mosh_core.cc:184`, `win32/mosh_core.cc:187`).
* **Status:** parity. The 15-second detection threshold and the transition into the bounded shutdown protocol match.

#### An interrupt control event enters protocol shutdown; a typed Ctrl-C does not

* **Upstream:** SIGINT and SIGTERM start network shutdown when a remote address exists (`src/frontend/stmclient.cc:508`). A typed Ctrl-C is not one of them: raw mode leaves terminal signal generation off, so the byte goes to the remote.
* **Port:** Setup clears `ENABLE_PROCESSED_INPUT` (`win32/console_io.cc:908`), so a typed Ctrl-C reaches `ReadFile` as `0x03` and is forwarded as an ordinary user byte — the escape key is `0x1e` (`win32/mosh_core.cc:114`), so nothing intercepts it. A **delivered** `CTRL_C_EVENT` or `CTRL_BREAK_EVENT` maps to `ShutdownCause::CTRL_BREAK` and signals the termination event (`win32/console_io.cc:655`, `win32/console_io.cc:661`, `win32/console_io.cc:676`); the owner observes it and begins graceful shutdown (`win32/console_io.cc:1243`, `win32/console_io.cc:1134`).
* **Status:** parity for the interrupt path, on first signal. With a remote address, `MoshCore::begin_shutdown()` starts the protocol shutdown (`win32/mosh_core.cc:294`). A repeated event is idempotent: after the first observation the loop no longer polls the termination event, and `CTRL_BREAK` carries no deadline (`win32/console_io.cc:1174`, `win32/console_io.cc:1243`, `win32/console_io.cc:125`). A close event can still escalate the same shutdown by publishing its deadline.
* **What is verified and what is not:** the acceptance modes drive `request_shutdown()` directly, so they establish the cause-to-shutdown mapping and its idempotence, not the delivery of a real control event. Keyboard forwarding of `0x03` rests on the cleared mode flag, not on a test.
* **Windows-only exit path:** clearing `ENABLE_PROCESSED_INPUT` suppresses Ctrl-C but not Ctrl-Break, so a *typed* Ctrl-Break does generate `CTRL_BREAK_EVENT` and ends the session locally. Upstream has no equivalent key.

#### The close control handler waits for the restoration attempt

* **Port:** `console_control_handler` publishes the close deadline and waits for `control->restored` for the remaining OS budget (`win32/console_io.cc:655`, `win32/console_io.cc:676`, `win32/console_io.cc:681`); `Impl::release_and_signal` attempts restoration and signals that event (`win32/console_io.cc:973`, `win32/console_io.cc:982`, `win32/console_io.cc:984`).
* **Status:** parity with the required bounded close-handling contract, for the paths that reach teardown. Returning `TRUE` alone cannot preserve the restoration-attempt window: Microsoft's [HandlerRoutine documentation](https://learn.microsoft.com/windows/console/handlerroutine) says the system terminates the process when `HandlerRoutine` returns `TRUE` or when the timeout expires. A signalled `restored` event means the attempt finished, not that it succeeded; the cleanup report holds that result. Waiting is therefore required, not defensive.
* **Limit:** the handler bounds its own wait, which is not the same as bounding the owner. An owner blocked in a console write never reaches teardown at all — see A24.

#### The cached timestamp is frozen after the wait

* **Upstream:** `Select::select()` freezes the timestamp after `pselect` returns (`src/util/select.h:186`).
* **Port:** `run_loop()` calls `core.refresh_clock()` after `WaitForMultipleObjects` returns and before any dispatch (`win32/console_io.cc:1223`); `MoshCore::refresh_clock` freezes the shared timestamp (`win32/mosh_core.cc:433`).
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
* **Port:** The supported invocation is `mosh.exe [-v ...] <user@host>` — zero or more value-free, non-credential-bearing verbosity flags plus exactly one destination — which obtains the key from the server's `MOSH CONNECT` reply over the `ssh` pipe. An endpoint is never accepted positionally: `parse_invocation` routes every argument shape other than optional verbosity flags and a single destination to usage (`win32/mosh_bootstrap.cc:377`, `win32/mosh_main.cc:143`), so no supported invocation asks a user to put a key there and nothing in the program reads one from `argv`. The only accepted option is the value-free `-v`; it carries no credential.
* **The property, stated so it can be enforced:** no interface requires or interprets a session key from `argv`; the server-issued key first exists after every child command line has been built (`build_ssh_command_line` takes only the ssh path and the destination, `win32/mosh_bootstrap.cc:165`, and is called before the reply is read, `win32/mosh_bootstrap.cc:410`); and `mosh.exe` never writes the key back into an argument, an environment block, or a diagnostic of its own — the child inherits stderr, so what `ssh.exe` prints is the child's to answer for, and S2 disposes of that child. What the program cannot do is keep a caller from typing a secret into the destination argument itself — that string is untyped, and it is copied into both this command line and `ssh.exe`'s. Argument classification governs what the program *accepts and reads*, not what a caller can place in a process parameter block that Windows populates before `main` runs.
* **What this record does not cover:** only `mosh.exe`'s own command line. `ssh.exe` necessarily holds the key too — it decrypts and buffers the `MOSH CONNECT` line before writing it to the pipe — and it is selected from `PATH`, so its provenance and its crash behavior are part of the same trust boundary and are governed by neither this repair nor anything `mosh.exe` can establish about a child it did not build. S2 now names it and disposes of it: a PATH-resolved OpenSSH client is a trusted external dependency, with the residual risk accepted.
* **Status:** parity on the property that matters — no supported path puts the session key where a Windows process parameter block would hold it, because such a block cannot be cleared once read. The mechanism differs deliberately. Upstream's environment channel does not transfer: a user who sets `MOSH_KEY` in a shell leaves the key in a long-lived parent the client cannot reach, and a child cannot establish dump protection before its own environment block exists, so the key is present during loader and startup failures. The pipe handoff avoids both.
* **Verification:** the classifier rejects endpoint-shaped argument lists (`win32/test_bootstrap.cc:322`), and CI asserts that the binary it builds refuses such an invocation with the usage code and that its usage text advertises no key, prediction mode, or positional endpoint. The CI probe uses a *valid* 22-character key, because `Base64Key` rejects a malformed one and an invocation refused by key validation would look identical to one refused by argument classification.
* **What the verification proves:** that the retired syntax stays retired. It is a regression guard, not a proof that no credential can reach a command line — it would not catch a future `--key` option, a differently named credential argument, or a key interpolated into some later child's command line. The invariants that would need their own checks are that the accepted grammar contains only value-free verbosity flags and a single destination (no credential-bearing option or operand), and that every child command line is built before the server-issued key exists. The check with the right shape is a fixture that plants a unique sentinel in place of the server's key, records every spawned child's command line and environment block, captures everything `mosh.exe` writes, and asserts the sentinel appears only on the path that consumes it. Routing all production process creation through one launcher would make that assertion hold for children added later, instead of only the one that exists now.
* **If a direct-endpoint facility is ever wanted again:** the retired one was advertised in the usage text, so removing it is a deliberate breaking change and not the deletion of unreachable development code. It never reached a release, which is the evidence that matters and is stronger than "no consumer is known": the fork publishes no releases, and the only tag carrying a `win32/` tree is `attic/task-4a-v1`, a development snapshot that does advertise the form (`win32/mosh_main.cc:72` there). Tagged, then, but never shipped. One workflow still left with it. Supplying an endpoint directly was the only way to reach a host whose server-side `SSH_CONNECTION` address the client cannot route to — behind NAT or a load balancer — since `mosh.exe <user@host>` derives its UDP target from that address. Those topologies are out of scope by decision, recorded under I9; the workflow is not replaced, it is withdrawn. A jump-only host is a different case with a different status: refused by the pinned proxy options, policy unresolved, tracked as I10. What the removal genuinely costs nothing is the rest: an ordinary session is `mosh.exe <user@host>`, and a synthetic endpoint is constructed in-process by the unit tests, which never build a command line. Restoring that reachability would not require restoring this interface, and the two should not be conflated: an address is routing metadata, only the key is secret. An endpoint override that keeps the ssh bootstrap for the key, or upstream's own client-visible-address discovery (`local` or `proxy`), would serve the withdrawn topologies with nothing secret in `argv` at all. I9 holds what such an override would have to cover. That is the shape to reach for if the scope decision is ever revisited. What follows applies only to the narrower case of a session established with no bootstrap at all. Should such a facility become necessary, it must take the key over a channel whose contents the receiving process can bound: an inherited pipe, or an equivalent handoff that happens after the process has started. Not an environment variable, not an interactive paste, and not another command-line spelling. The command line is the settled one: whatever its spelling, it lands in a process parameter block that cannot be cleared once read, which is precisely what this record repaired — a `--key` option would reintroduce it exactly. The other two are ruled out under the threat model this repair assumed — a same-user reader — rather than absolutely. An environment variable inherited from the user's shell sits in a long-lived parent the client cannot scrub, and is present in the child's own block during loader and startup failures, before the child can protect itself; an ephemeral launcher would avoid the first half of that but not the second. An interactive paste puts the key through console input and screen buffers whose lifetime the reader does not control by default; no-echo input narrows that, and a design that relies on it has to say so and show it. The channel is the constraint; the protocol is not designed here. A real proposal would still have to settle who obtains the key, how the child identifies the handle it was given, handle-inheritance limits, cancellation and timeout, and zeroization on both sides — none of which can be chosen sensibly before a consumer exists.
* **Residual:** the bootstrap's intermediate key copies — the parsed reply and the pipe drain buffers — are freed without being zeroed, so a crash-dump-class adversary can still recover the key from freed heap. This is not a divergence: upstream's `mosh-client` zeroes none of its key copies either. Crash-dump exposure itself is S2, which is unaffected by this repair.

#### A receive pass over a fresh client socket finds no packet, not an error (repaired)

* **Upstream:** A POSIX `recvmsg` on an unbound UDP socket reports `EAGAIN`, so a receive pass over a client socket that has not sent yet — a fresh port-hop socket during a roam — finds no packet (`src/network/network.cc:681`).
* **Port:** Winsock fails `recvfrom` on an unbound socket with `WSAEINVAL`, which is outside the nonfatal continuation list (`src/network/network.cc:542`), so the same pass surfaced "recvfrom: An invalid argument was supplied" in the notification overlay until the next `sendto` auto-bound the socket. Client sockets are now bound to the wildcard address with port 0 at creation (`bind_wildcard`, `src/network/network.cc:114`), called from `hop_port` and the client constructor (`src/network/network.cc:169`, `src/network/network.cc:484`); the port number remains kernel-chosen, but its allocation moves from the first `sendto` to bind time, so ephemeral-port exhaustion would surface at connection setup or a hop rather than at transmission. The bind happens before the socket joins the live deque, so a failed hop keeps the previous working socket instead of publishing an unbound one. Server sockets are excluded: `try_bind` binds them to a specific port, and Winsock rejects `bind` on an already-bound socket.
* **Status:** repaired. A receive pass over a fresh client socket finds no packet on both platforms.
* **Verification:** `test_loopback` receives from a freshly constructed client `Connection` before any send and asserts the exact public no-packet outcome (`NetworkException` with errno 0; `Connection::recv()` consumes per-socket would-block internally), then drives a real port hop — waits out `PORT_HOP_INTERVAL`, re-freezes the cached timestamp, sends so the hop appends a fresh unsent socket — and repeats the assertion while the old and new sockets coexist, followed by a post-hop round trip (`expect_no_packet`, `win32/test_loopback.cc:116`, called at `win32/test_loopback.cc:151` and `win32/test_loopback.cc:168`). CI runs it on native ARM64, where it reproduced the `WSAEINVAL` before the repair.

#### A repeatedly requested zero wait is bounded (A18, implemented; loop-integration verification pending A26)

* **Upstream:** `Select::select()` counts consecutive zero-timeout calls and raises the timeout to 1 ms from the tenth onward, clearing the count when a nonzero timeout appears (`src/util/select.h:131`, `MAX_POLLS = 10` at `src/util/select.h:230`).
* **Port:** `PollThrottle::bound` applies the same rule — the first nine consecutive zero requests pass through, the tenth and every zero after it return 1 ms, and a nonzero request passes through and clears the count (`win32/console_io.cc:757`). `run_loop()` holds one per session and applies it to the interval the sources request (`win32/console_io.cc:1201`).
* **Status:** implemented, and behaviorally at parity on the rule — the two agree call for call over the range upstream defines. Verification is split deliberately: the rule is covered, its use by the loop is not, and A26 holds that. The record sits here rather than in the findings because the port no longer spins; a reader looking up whether the defect is live must not be told it is. One deliberate difference: the port's count saturates at ten where upstream's keeps incrementing a signed `int` that would eventually overflow. Saturation changes no result and hardens the port.
* **Scope:** the throttle covers the requested interval only, and it bounds the timeout rather than the iteration rate — a wait whose handle is already signalled returns at once, as upstream's `pselect` does. The termination-deadline clamp below it is excluded on purpose, because it drives to zero as the restoration reserve approaches and a floor there would only delay the loop's exit: that zero costs a single wait, since `restoration_reserve_reached` applies the identical predicate to the same deadline (`win32/console_io.cc:1146`, `win32/console_io.cc:1205`) and is re-evaluated just past the wait, breaking the loop in that same iteration ahead of the resize, socket, input, and frame work (`win32/console_io.cc:1247`). It is not ahead of *everything*: the termination check precedes it (`win32/console_io.cc:1243`), so a shutdown request first observed on this wakeup performs its state transition — status, notification, `start_shutdown()` — inside the reserve before the break. Those are state assignments rather than I/O, and this ordering predates the throttle and is unchanged by it, but the reserve is a restoration budget and what may run inside it should be stated rather than assumed. The deadline it reads only ever tightens — the sole publisher takes the earlier of the two candidates (`win32/console_io.cc:157`) — so a clamp that has fired cannot be undone by a later publication. The reader's teardown wait is not throttled and does not need to be: it returns outright when its remaining budget reaches zero rather than waiting on it (`win32/console_io.cc:559`).
* **Verification:** a unit test drives `PollThrottle` across the boundary, across a reset by a nonzero request both before and after the floor engages, and well past the boundary, which is what would catch a counter that wraps or resets itself into another burst of zero waits (`win32/test_console_lifecycle.cc:1187`). CI runs it on native ARM64, and a separate CI step asserts that the loop's sole wait takes its timeout from the throttle.
* **Limit, and it is the substantive one:** nothing executable covers the loop's *use* of the throttle. The unit test proves arithmetic. The CI step compares source text against the four lines this wiring occupies, so it is a deletion alarm, not a proof: it does not establish data flow or ordering, and a rewrite that reaches a wait by another route defeats it. The finding's original acceptance criterion — drive the loop with sustained zero requests and observe the wait floor — is therefore **not met**, and no harness can meet it today. A26 holds that gap and the criterion that would close it.

#### Astral-plane characters decode to one wide cell (repaired)

* **Upstream:** On Linux `wchar_t` is 32-bit, so `mbrtowc()` decodes a 4-byte UTF-8 character (an emoji, or CJK Ext B and later) into a single `wchar_t` holding the whole Unicode scalar. The parser feeds that one value through `Parser::input`, `Emulator::print` calls `wcwidth` on it (2 for the wide blocks), and `Cell::append` re-encodes it with `wcrtomb` into the cell's UTF-8 bytes (`src/terminal/parser.cc`, `src/terminal/terminal.cc`, `src/terminal/terminalframebuffer.h`).
* **Port:** Windows `wchar_t` is a 16-bit UTF-16 code unit, so it cannot carry an astral scalar, and UCRT `mbrtowc` proved unusable for reassembling the surrogate pair (a full 4-byte sequence decoded to U+FFFD; fed incrementally it reported incomplete at every stage). The repair has two parts. First, because the port forces a UTF-8 locale at startup (`win32/mosh_core.cc`), `mosh_mbrtoc32` decodes UTF-8 to a scalar directly, rejecting overlong forms and encoded surrogates as ill-formed; `mosh_c32rtomb` encodes a scalar as UTF-8 for cell storage; `mosh_win32_wcwidth_scalar` classifies astral width against the glibc East-Asian Width table (wide, narrow, and zero-width ranges) (`win32/wincompat.cc`, `win32/wcwidth.h`). Second — the part that actually moves the scalar — the engine's decoded-character channel is widened from `wchar_t` to `char32_t` end to end: `Parser::input`, `Action::ch`, the `parserstate` input rules, `Emulator::print`, `Cell::append`/`append_to_str`, and the OSC/dispatch strings. On POSIX `wchar_t` is already 32-bit, so `char32_t` is behavior-identical there and the only casts are lossless `char32_t`↔`wchar_t` at the `mbrtowc`/`wcrtomb`/`wcwidth` RTL boundary; on Windows the `#ifdef _WIN32` width and encode calls use the `mosh_*` scalar shims. Genuine UTF-16 data — the window-title/icon/clipboard `title_type` and the notification `std::wstring`/`swprintf` buffers — stays `wchar_t`.
* **Consequence:** Emoji and astral CJK now render as one wide cell holding the original UTF-8 bytes, matching upstream on Linux. BMP behavior (narrow, wide, combining, unprintable) is unchanged. Prediction classifies the width of the scalar, not a 16-bit code unit: `terminaloverlay.cc` routes any non-width-1 character to `become_tentative()` using the same scalar width classifier as the emulator (`mosh_win32_wcwidth_scalar` on Windows, `wcwidth` on POSIX), so an astral character is no longer misjudged as narrow (U+1F600 truncated to 0xF600 is PUA width 1) and the server framebuffer stays authoritative. `mosh_win32_wcwidth_scalar`'s astral table mirrors glibc 2.39 `C.UTF-8` `wcwidth` exactly for every astral scalar, in all four width classes: -1 (noncharacters and unassigned), 0 (combining/format), 1 (narrow), 2 (East-Asian Wide). The table is generated, not hand-maintained: `win32/gen_wcwidth_tables.sh` probes the pinned glibc oracle over every astral scalar and emits the sorted interval table checked into `win32/wcwidth.h`, and its `--check` mode fails if the checked-in table drifts from the oracle. An exhaustive harness comparing the header against glibc over all astral scalars reports zero mismatches.
* **Status:** repaired. The engine's one-scalar-per-character invariant is preserved by making the character type `char32_t`, which is what upstream's `wchar_t` already is on Linux.
* **Upstream-merge note:** this is a semantic divergence, not a shim — thirteen upstream files carry `char32_t` where upstream has `wchar_t`. The change is mechanical signature widening (the logic is unchanged), so an upstream merge that touches these lines conflicts textually but resolves by keeping `char32_t`. These are stable terminal-emulator core files that upstream rarely modifies, so the merge frequency is low; and if upstream ever adopts `char32_t` itself, this divergence disappears. Do not "resolve" a future conflict by reverting to `wchar_t` — that reintroduces the 16-bit truncation on Windows.
* **Verification:** `win32/test_terminal_width.cc` asserts the astral width classification and drives a 4-byte UTF-8 emoji through the real `UTF8Parser` into an `Emulator`, requiring one wide cell with the original bytes and a two-column cursor advance; CI runs it on native ARM64. A RED-probe branch that reverted only the decode/encode to raw `mbrtowc`/`wcrtomb` failed that assertion on native ARM64 (`get_cursor_col() == 2`), confirming the test discriminates the fix.

## Seeds checked against the code

* `CONNECTION_TIMEOUT = 15000` is not an invented value. Upstream uses the same literal (`src/frontend/stmclient.cc:539`). The threshold and transition match.
* Timestamp placement is current parity, not a divergence.
* The resize seed's direction was backwards. It is the **port** that skips the prediction reset during shutdown, not upstream — see A8.
