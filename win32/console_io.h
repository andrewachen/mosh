/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give
    permission to link the code of portions of this program with the
    OpenSSL library under certain conditions as described in each
    individual source file, and distribute linked combinations including
    the two.

    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do
    so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

/* ABOUTME: Windows-console I/O layer for mosh.exe: raw-mode/UTF-8 setup, teardown, and event loop. */
/* ABOUTME: Keeps Win32 console calls out of the OS-agnostic MoshCore. */
#ifndef CONSOLE_IO_H
#define CONSOLE_IO_H

/* winsock2 must precede windows.h, which would otherwise include legacy winsock.h. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include "mosh_core.h"
#include <windows.h>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

struct ConsoleError : public std::runtime_error {
  DWORD win32_code;
  ConsoleError( DWORD code, const std::string &msg )
    : std::runtime_error( msg ), win32_code( code ) {}
};

void console_dims( int *cols, int *rows );

/* Write every byte or throw ConsoleError. */
void write_all( HANDLE handle, const std::string &bytes );

/* Why a session is ending. IN_BAND is the default/benign cause (e.g. the
   remote side quit); the rest name an external event that interrupted the
   owner thread. */
enum class ShutdownCause {
  IN_BAND,
  CTRL_BREAK,
  IO_LOSS,
  CTRL_CLOSE,
  SESSION_END,
};

/* Where a session is in its life. RUNNING covers the whole time run() is
   pumping; RESTORED means the console has been given back regardless of
   whether every restore step succeeded; DONE is teardown-complete. */
enum class ConsoleLifecycleState {
  RUNNING,
  SHUTTING_DOWN,
  RESTORED,
  DONE,
};

/* Console state captured before setup mutates it, and restored from at
   teardown. */
struct ConsoleSnapshot {
  HANDLE input;
  HANDLE output;
  DWORD input_mode;
  DWORD output_mode;
  UINT input_cp;
  UINT output_cp;
};

/* Which restore step (if any) first failed. Shared between console_io.cc,
   which records it, and mosh_main.cc, which formats it, so the two cannot
   drift out of sync if the steps are ever renumbered. */
enum CleanupOp {
  CLEANUP_OP_NONE,
  CLEANUP_OP_CLOSE_SEQUENCE,
  CLEANUP_OP_OUTPUT_CP,
  CLEANUP_OP_INPUT_CP,
  CLEANUP_OP_OUTPUT_MODE,
  CLEANUP_OP_INPUT_MODE,
};

/* Records the first Win32 failure encountered while restoring the console, if
   any. Later failures during the same restore are not recorded. */
struct CleanupReport {
  DWORD first_error;
  int failed_op;
  DWORD reader_error;
};

/* A console setup mutation the test-only failure injector can target, in the
   order setup applies them. */
enum class ConsoleSetupStep {
  INPUT_MODE,
  OUTPUT_MODE,
  INPUT_CODE_PAGE,
  OUTPUT_CODE_PAGE,
};

/* Test-only: makes setup fail immediately after the named mutation succeeds,
   to exercise rollback of that specific mutation. */
void console_test_fail_after( ConsoleSetupStep step );

/* Test-only: signals the real termination event after the named mutation
   succeeds. Setup then stops at the production termination checkpoint rather
   than at an injected failure, so deleting that checkpoint fails the test. */
void console_test_signal_termination_after( ConsoleSetupStep step );

/* Test-only reader outcomes used by the console lifecycle acceptance harness. */
enum class ConsoleReaderTestOutcome {
  NONE,
  END_OF_INPUT,
  READ_FAILURE,
  NONTERMINATING,
  /* Simulates an old-conhost Ctrl-Z: the first read completes as a successful
     zero-byte read; subsequent reads block on real console input. */
  ZERO_BYTE_READ,
};
void console_test_set_reader_outcome( ConsoleReaderTestOutcome outcome );

/* Test-only: overrides the OS shutdown budget used for deadline publication.
   Zero clears the override. */
void console_test_set_shutdown_budget( DWORD budget_ms );

/* Test-only: pauses teardown after reader stop has begun waiting, so acceptance
   can publish a close request during that potentially unbounded operation. */
void console_test_pause_teardown( HANDLE entered, HANDLE resume );

/* Test-only: disarms all process-global, sticky injectors. */
void console_test_clear_setup_injections();

/* Test-only: the restored event of the most recently created session. Readable
   after a constructor throws, when no session object survives to be asked.
   Control blocks and their handles are never reclaimed, so this stays valid. */
HANDLE console_test_last_restored_event();

/* Installed via SetConsoleCtrlHandler. Runs on a system-owned thread that may
   already be active when the session is torn down; it only records the cause
   and signals termination, never touching console or MoshCore state itself. */
BOOL WINAPI console_control_handler( DWORD type );

/* Bounds how often the loop may be asked to poll. A zero wait time is a
   legitimate request: a source has work due now. Sustained zeros are not,
   because nothing else in the loop bounds how often it may re-ask, so a source
   that stays due spins a core. Counting consecutive zero requests and imposing
   a 1 ms floor from the tenth onward leaves a genuine poll burst free of added
   latency while giving a stuck source somewhere to block. The floor bounds the
   requested timeout, not the iteration rate: a wait whose handle is already
   signalled still returns at once, exactly as upstream's pselect does.
   Upstream applies the identical rule in Select::select()
   (src/util/select.h:131, MAX_POLLS = 10). */
class PollThrottle {
public:
  PollThrottle() : consecutive_polls( 0 ) {}
  /* The wait to use for a caller asking for `requested`. A nonzero request
     passes through and clears the count. */
  DWORD bound( DWORD requested );
  static const unsigned MAX_CONSECUTIVE_POLLS = 10;
private:
  unsigned consecutive_polls;
};

/* Owns console setup/teardown and the console event loop. The constructor
   performs every console mutation, installs the control handler, starts the
   reader, and writes the open sequence; by the time it returns the session is
   fully live. run() assumes that and performs no setup of its own. */
class ConsoleSession {
public:
  explicit ConsoleSession( MoshCore &core );
  ~ConsoleSession();
  ConsoleSession( const ConsoleSession & ) = delete;
  ConsoleSession &operator=( const ConsoleSession & ) = delete;

  /* Main event loop. Per wakeup it:
       - refreshes the cached timestamp after the wait returns
       - services termination (calls begin_shutdown() once when observed)
       - polls for a resize
       - at most one readable socket
       - one bounded input chunk
       - writes any pending frame
     The loop continues until core.is_finished() returns true. A shutdown
     request begins a graceful shutdown: begin_shutdown() is called once and
     the loop keeps pumping so the transport can complete its shutdown handshake.

     Returns when core.is_finished() is true after core.tick() at the top of an
     iteration. Restores the console on every exit path, normal or
     exceptional, before returning or propagating.

     Otherwise it throws rather than returning. ConsoleError comes from socket
     reconciliation, the handle-count guard, the wait itself, the reader's
     deferred ReadFile failure, the resize query, socket event enumeration, and
     frame output; core.tick() and core.on_readable() additionally rethrow a
     fatal Crypto::CryptoException. Both derive from std::exception, which is
     what callers should catch. */
  void run();

  /* The sole cross-thread entry point: records the cause and signals
     termination. Everything else stays on run()'s owner thread. */
  void request_shutdown( ShutdownCause cause );

  /* Test-only accessors for the event-loop acceptance harness. Every one of
     these is meaningful as soon as the constructor returns, before run() is
     ever called. */
  HANDLE termination_event_for_test() const;
  HANDLE restored_event_for_test() const;
  ConsoleSnapshot original_console_for_test() const;
  size_t input_backlog_for_test() const;
  /* True if a drain ever left the input queue empty. Records the drain event
     itself rather than an end state, so it cannot be confused by whatever the
     queue happens to hold when run() returns. */
  bool input_ever_drained_for_test() const;
  /* True once the event loop has observed a shutdown request and begun the
     graceful shutdown. Read only after run() returns; this value is written by
     run() without synchronization. */
  bool shutdown_observed_for_test() const;
  /* Returns zero when no deadline-bearing cause was ever published. */
  ULONGLONG termination_deadline_for_test() const;
  /* Whether a drain had ever emptied the input queue at the moment the loop
     first observed a shutdown request. Sampled at that instant rather than
     after run() returns, because the loop keeps pumping afterward and will
     drain the queue in the normal course of shutting down. Read only after
     run() returns; this value is written by run() without synchronization. */
  bool input_drained_at_shutdown_for_test() const;
  /* One per event-loop iteration: the cached timestamp as the wait ended
     (tick_ts, still the value core.tick() froze before the wait) and the cached
     timestamp after core.refresh_clock() (dispatch_ts, what every source
     dispatched this iteration sees). A loop that never refreshes leaves the two
     equal on every iteration. */
  struct ClockRefreshSample {
    uint64_t tick_ts;
    uint64_t dispatch_ts;
  };
  /* Copies up to `capacity` samples into `out` and returns how many were
     written. Recording stops at a fixed capacity, so these are the first
     iterations. The caller must not run this concurrently with run(): unlike the
     accessors above it takes no lock, because the samples are written from
     inside run() without one. */
  size_t clock_refresh_samples_for_test( ClockRefreshSample *out, size_t capacity ) const;
  static size_t input_budget_bytes();
  /* Holds the reader queue mutex across both the backlog check and the
     termination signal, so the backlog is read consistently against a queue the
     reader thread is still growing. Returns false if the backlog is short.
     Non-blocking — the caller establishes the backlog. */
  bool signal_termination_against_backlog_for_test( size_t minimum );
  /* Report from the most recent restore attempt (constructor rollback,
     run()'s exit path, or the destructor). CLEANUP_OP_NONE / ERROR_SUCCESS
     until a restore has actually run. */
  CleanupReport cleanup_report() const;
private:
  class Impl;
  std::unique_ptr<Impl> impl;
};
#endif
