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

/* ABOUTME: Acceptance harness for the native Windows console lifecycle, run on the inherited console. */
/* ABOUTME: Modes: vt-mode asserts setup/restore console state; fairness proves termination wins a contested wakeup; clock-refresh proves the timestamp refreshes after the wait; transaction proves rollback after an injected setup failure; init-checkpoint proves it after a real mid-setup shutdown. */

#include <atomic>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <thread>

#include "src/frontend/terminaloverlay.h"
#include "win32/console_io.h"
#include "win32/mosh_core.h"
#include "win32/test_server.h"

/* TestServer lives in namespace Terminal so Terminal::Cell can befriend it. */
using Terminal::TestServer;

class ConsoleTestScope;
static ConsoleTestScope *active_console_scope = NULL;
static void must( BOOL ok, const char *what );

static StartupOptions never_prediction()
{
  StartupOptions opts;
  opts.predict_display = Overlay::PredictionEngine::Never;
  return opts;
}

/* Point the process std handles at the real attached console for the duration
   of an in-process test. The msys2 CI shell redirects std handles to pipes, so
   GetStdHandle(STD_OUTPUT_HANDLE) is not the console; CONOUT$/CONIN$ always name
   the attached console. ConsoleSession's constructor reads GetStdHandle, so
   repointing the std handles lets it run its production path against the real
   console. stderr is deliberately left untouched so failure diagnostics still
   reach the CI log. */
class ConsoleTestScope {
public:
  ConsoleTestScope()
  {
    conout_ = CreateFileW( L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL );
    must( conout_ != INVALID_HANDLE_VALUE, "CreateFileW(CONOUT$)" );
    conin_ = CreateFileW( L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                          OPEN_EXISTING, 0, NULL );
    must( conin_ != INVALID_HANDLE_VALUE, "CreateFileW(CONIN$)" );
    orig_out_ = GetStdHandle( STD_OUTPUT_HANDLE );
    orig_in_ = GetStdHandle( STD_INPUT_HANDLE );
    must( GetConsoleMode( conout_, &orig_out_mode_ ), "GetConsoleMode(conout)" );
    must( GetConsoleMode( conin_, &orig_in_mode_ ), "GetConsoleMode(conin)" );
    orig_out_cp_ = GetConsoleOutputCP();
    must( orig_out_cp_ != 0, "GetConsoleOutputCP" );
    orig_in_cp_ = GetConsoleCP();
    must( orig_in_cp_ != 0, "GetConsoleCP" );
    active_console_scope = this;
    must( SetStdHandle( STD_OUTPUT_HANDLE, conout_ ), "SetStdHandle(out)" );
    must( SetStdHandle( STD_INPUT_HANDLE, conin_ ), "SetStdHandle(in)" );
  }
  ~ConsoleTestScope()
  {
    restore();
    if ( active_console_scope == this ) {
      active_console_scope = NULL;
    }
    CloseHandle( conout_ );
    CloseHandle( conin_ );
  }
  /* Restore console-wide state (modes, code pages, std handles) to the values
     captured at construction, WITHOUT closing the console handles. Idempotent.
     Callable before std::_Exit on the watchdog-deadline path, where a detached
     owner thread is still using these handles — closing them (as the destructor
     does) would break that thread, and unwinding to run the destructor would
     also destroy the core/session state it depends on.

     Also discards pending console input, which is not restoration: fairness mode
     writes a large backlog of synthetic keystrokes into the shared CI console's
     input buffer to establish the reader backlog, and whatever the Reader has
     not consumed would otherwise be typed ahead into whatever runs next. Once
     run() is entered the fill is done and the Reader only consumes, so no
     further keystrokes reach that buffer. The watchdog's own start-wait failure
     path is the exception: it can restore while the fill loop is still writing,
     leaving records behind the flush. */
  void restore()
  {
    if ( restored_ ) {
      return;
    }
    SetConsoleMode( conout_, orig_out_mode_ );
    SetConsoleMode( conin_, orig_in_mode_ );
    SetConsoleOutputCP( orig_out_cp_ );
    SetConsoleCP( orig_in_cp_ );
    FlushConsoleInputBuffer( conin_ );
    SetStdHandle( STD_OUTPUT_HANDLE, orig_out_ );
    SetStdHandle( STD_INPUT_HANDLE, orig_in_ );
    restored_ = true;
  }
  ConsoleTestScope( const ConsoleTestScope & ) = delete;
  ConsoleTestScope &operator=( const ConsoleTestScope & ) = delete;
private:
  HANDLE conout_;
  HANDLE conin_;
  HANDLE orig_out_;
  HANDLE orig_in_;
  DWORD orig_out_mode_;
  DWORD orig_in_mode_;
  UINT orig_in_cp_;
  UINT orig_out_cp_;
  bool restored_ = false;
};

/* NDEBUG-proof check for load-bearing Win32 setup calls: restores the shared
   console before aborting rather than being compiled out like assert(). */
static void must( BOOL ok, const char *what )
{
  if ( !ok ) {
    fprintf( stderr, "fatal: %s failed (GetLastError=%lu)\n", what,
             GetLastError() );
    if ( active_console_scope != NULL ) {
      active_console_scope->restore();
    }
    abort();
  }
}

/* Returns nonzero on assertion failure so CI can detect a RED build. */
static int run_escape_key()
{
  TestServer server( 80, 24 );

  /* Custom control escape (Ctrl-A) + '.' begins shutdown. */
  {
    StartupOptions opts = never_prediction();
    opts.escape.key = 0x01;
    opts.escape.pass_key = 'A';
    opts.escape.pass_key2 = 'A';
    opts.escape.requires_lf = false;
    MoshCore core( "127.0.0.1", server.port().c_str(), server.get_key().c_str(), 80, 24, opts );
    const char quit[] = { 0x01, '.' };
    core.feed_input( quit, sizeof quit );
    if ( core.status_message() != "Exiting..." ) {
      fprintf( stderr, "FAIL: escape-key: custom escape did not begin shutdown (status=\"%s\")\n",
               core.status_message().c_str() );
      return 1;
    }
  }
  /* The former default 0x1e is now an ordinary byte. */
  {
    StartupOptions opts = never_prediction();
    opts.escape.key = 0x01;
    opts.escape.pass_key = 'A';
    opts.escape.pass_key2 = 'A';
    MoshCore core( "127.0.0.1", server.port().c_str(), server.get_key().c_str(), 80, 24, opts );
    const char seq[] = { 0x1e, '.' };
    core.feed_input( seq, sizeof seq );
    if ( !core.status_message().empty() ) {
      fprintf( stderr, "FAIL: escape-key: non-escape 0x1e began shutdown\n" );
      return 1;
    }
  }
  /* Disabled parser (key == -1): no byte begins shutdown. */
  {
    StartupOptions opts = never_prediction();
    opts.escape.key = -1;
    MoshCore core( "127.0.0.1", server.port().c_str(), server.get_key().c_str(), 80, 24, opts );
    const char seq[] = { 0x1e, '.' };
    core.feed_input( seq, sizeof seq );
    if ( !core.status_message().empty() ) {
      fprintf( stderr, "FAIL: escape-key: disabled parser began shutdown\n" );
      return 1;
    }
  }
  return 0;
}

static int run_no_term_init()
{
  TestServer server( 80, 24 );
  {
    StartupOptions opts = never_prediction();
    opts.no_term_init = false;
    MoshCore core( "127.0.0.1", server.port().c_str(), server.get_key().c_str(), 80, 24, opts );
    if ( core.open_sequence().find( "\033[?1049h" ) == std::string::npos
         || core.close_sequence().find( "\033[?1049l" ) == std::string::npos ) {
      fprintf( stderr, "FAIL: no-term-init: alternate screen missing by default\n" );
      return 1;
    }
  }
  {
    StartupOptions opts = never_prediction();
    opts.no_term_init = true;
    MoshCore core( "127.0.0.1", server.port().c_str(), server.get_key().c_str(), 80, 24, opts );
    if ( core.open_sequence().find( "\033[?1049h" ) != std::string::npos
         || core.close_sequence().find( "\033[?1049l" ) != std::string::npos ) {
      fprintf( stderr, "FAIL: no-term-init: alternate screen present when suppressed\n" );
      return 1;
    }
  }
  return 0;
}

static int run_vt_mode()
{
  ConsoleTestScope scope;   /* std handles now name the real console */

  HANDLE h_out = GetStdHandle( STD_OUTPUT_HANDLE );
  DWORD initial_out = 0;
  must( GetConsoleMode( h_out, &initial_out ), "GetConsoleMode(h_out)" );
  must( SetConsoleMode( h_out, initial_out & ~ENABLE_PROCESSED_OUTPUT ), "SetConsoleMode(clear PROCESSED_OUTPUT)" );

  TestServer server( 80, 24 );
  MoshCore core( "127.0.0.1", server.port().c_str(), server.get_key().c_str(),
                 80, 24, never_prediction() );

  ConsoleSnapshot snapshot;
  DWORD active_out_mode = 0;
  {
    ConsoleSession session( core );
    snapshot = session.original_console_for_test();
    must( GetConsoleMode( snapshot.output, &active_out_mode ), "GetConsoleMode(active)" );
  }
  /* session is out of scope here: the destructor has already restored. */
  if ( ( active_out_mode & ENABLE_PROCESSED_OUTPUT ) == 0 ) {
    fprintf( stderr, "FAIL: ENABLE_PROCESSED_OUTPUT not set\n" );
    return 1;
  }
  if ( ( active_out_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING ) == 0 ) {
    fprintf( stderr, "FAIL: ENABLE_VIRTUAL_TERMINAL_PROCESSING not set\n" );
    return 1;
  }
  if ( ( active_out_mode & DISABLE_NEWLINE_AUTO_RETURN ) == 0 ) {
    fprintf( stderr, "FAIL: DISABLE_NEWLINE_AUTO_RETURN not set\n" );
    return 1;
  }
  /* All four mutations, not just the output mode: this is the only mode that
     exercises the destructor's restore after a construction that succeeded, so
     anything it does not compare is unverified anywhere. */
  DWORD restored_out_mode = 0;
  DWORD restored_in_mode = 0;
  must( GetConsoleMode( snapshot.output, &restored_out_mode ), "GetConsoleMode(restored out)" );
  must( GetConsoleMode( snapshot.input, &restored_in_mode ), "GetConsoleMode(restored in)" );
  const UINT restored_out_cp = GetConsoleOutputCP();
  const UINT restored_in_cp = GetConsoleCP();
  if ( restored_out_mode != snapshot.output_mode
       || restored_in_mode != snapshot.input_mode
       || restored_out_cp != snapshot.output_cp
       || restored_in_cp != snapshot.input_cp ) {
    fprintf( stderr,
             "FAIL: console not fully restored (out_mode=%lu/%lu in_mode=%lu/%lu "
             "out_cp=%u/%u in_cp=%u/%u)\n",
             restored_out_mode, snapshot.output_mode, restored_in_mode,
             snapshot.input_mode, restored_out_cp, snapshot.output_cp,
             restored_in_cp, snapshot.input_cp );
    return 1;
  }
  return 0;
}

/* Deadline for the fairness harness to allow the transport shutdown handshake
   to finish. It exceeds the upstream ACTIVE_RETRY_TIMEOUT (10000 ms) and
   SHUTDOWN_RETRIES (16), so the watchdog does not reject a healthy shutdown. */
static const DWORD FAIRNESS_DEADLINE_MS = 30000;
/* Timeout for establishing the backlog before run() is entered. Failing here
   is a harness setup failure, not a dispatch verdict. */
static const DWORD FAIRNESS_FILL_TIMEOUT_MS = 5000;
/* Batch size and batch cap for the synchronous backlog fill. Reaching
   INPUT_BUDGET_BYTES+1 takes roughly seventeen batches; the cap leaves ample
   headroom for a lagging reader while keeping a wedged one from letting the
   fill timeout alone push millions of records into the shared console input
   buffer. */
static const DWORD RECORDS_PER_FILL_BATCH = 4096;
static const size_t FAIRNESS_FILL_MAX_BATCHES = 64;

class UniqueHandle {
public:
  explicit UniqueHandle( HANDLE handle ) : handle_( handle ) {}
  ~UniqueHandle()
  {
    if ( handle_ != NULL ) {
      CloseHandle( handle_ );
    }
  }
  HANDLE get() const
  {
    return handle_;
  }
  UniqueHandle( const UniqueHandle & ) = delete;
  UniqueHandle &operator=( const UniqueHandle & ) = delete;
private:
  HANDLE handle_;
};

/* Enforces a mode's deadline from a separate thread so the test thread can be
   the one inside run(). stop() and the destructor both release the thread, so
   an early return needs no special handling. */
class WatchdogGuard {
public:
  WatchdogGuard( HANDLE session_done, HANDLE deadline_start,
                 const std::atomic<ULONGLONG> *deadline_at, ConsoleTestScope *scope,
                 DWORD deadline_ms )
    : session_done_( session_done ),
      thread_( &WatchdogGuard::watch, session_done, deadline_start, deadline_at, scope,
               deadline_ms )
  {}
  ~WatchdogGuard()
  {
    stop();
  }
  void stop()
  {
    SetEvent( session_done_ );
    if ( thread_.joinable() ) {
      thread_.join();
    }
  }
  WatchdogGuard( const WatchdogGuard & ) = delete;
  WatchdogGuard &operator=( const WatchdogGuard & ) = delete;
private:
  static void watch( HANDLE session_done, HANDLE deadline_start,
                     const std::atomic<ULONGLONG> *deadline_at, ConsoleTestScope *scope,
                     DWORD deadline_ms )
  {
    HANDLE handles[] = { deadline_start, session_done };
    const DWORD start_result = WaitForMultipleObjects( 2, handles, FALSE, INFINITE );
    if ( start_result == WAIT_OBJECT_0 + 1 ) {
      return;
    }
    if ( start_result != WAIT_OBJECT_0 ) {
      fprintf( stderr,
               "fatal: WaitForMultipleObjects(deadline_start, session_done) failed "
               "(result=%lu GetLastError=%lu)\n", start_result,
               start_result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS );
      scope->restore();
      std::_Exit( 1 );
    }

    /* Wait against the absolute deadline the caller stamped before entering
       run(), not a fresh interval. Creating this thread does not mean it has
       reached the wait above, so measuring from here would silently extend
       the bound by however long console contention delayed its first
       scheduling — the same latency this harness exists to stop measuring. */
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG expires_at = deadline_at->load();
    const DWORD remaining = now >= expires_at
      ? 0 : static_cast<DWORD>( expires_at - now );
    const DWORD wait_result = WaitForSingleObject( session_done, remaining );
    const DWORD wait_error = wait_result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    if ( wait_result == WAIT_OBJECT_0
         || WaitForSingleObject( session_done, 0 ) == WAIT_OBJECT_0 ) {
      return;
    }
    if ( wait_result == WAIT_TIMEOUT ) {
      fprintf( stderr, "FAIL: session.run() did not return within %lums\n",
               deadline_ms );
    } else {
      fprintf( stderr,
               "fatal: WaitForSingleObject(session_done) failed "
               "(result=%lu GetLastError=%lu)\n", wait_result, wait_error );
    }
    /* The calling thread is stuck in run(), so restore the console and exit
       without unwinding. Destroying core, server, or saved while run() uses
       them would be unsafe; _Exit leaves them valid until process termination. */
    scope->restore();
    std::_Exit( 1 );
  }

  HANDLE session_done_;
  std::thread thread_;
};

/* Fairness test: proves the event loop observes termination on the same wakeup
   as a ready reader event. A winner-only dispatch favors the reader event at
   index 0 whenever it is signaled, so termination cannot be observed on a
   contested wakeup. The fair dispatch independently re-tests termination.

   Synchronization contract: the test preloads the reader backlog to
   INPUT_BUDGET_BYTES+1 before run() begins, then atomically verifies the
   backlog and signals termination while holding the reader queue mutex. The
   sampled drain state captures whether a drain had already emptied the queue
   at the instant the shutdown request was observed. A fair dispatch samples
   false while the contested backlog remains; a winner-only dispatch samples
   true after draining the reader queue until termination wins an idle wakeup. */
static int run_fairness()
{
  ConsoleTestScope scope;

  TestServer server( 80, 24 );
  MoshCore core( "127.0.0.1", server.port().c_str(), server.get_key().c_str(),
                 80, 24, never_prediction() );

  UniqueHandle session_done( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  if ( session_done.get() == NULL ) {
    fprintf( stderr, "fatal: CreateEvent(session_done) failed (GetLastError=%lu)\n",
             GetLastError() );
    return 1;
  }
  UniqueHandle deadline_start( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  if ( deadline_start.get() == NULL ) {
    fprintf( stderr, "fatal: CreateEvent(deadline_start) failed (GetLastError=%lu)\n",
             GetLastError() );
    return 1;
  }

  auto session = std::make_unique<ConsoleSession>( core );
  const HANDLE h_in = session->original_console_for_test().input;
  /* Stamped immediately before run() is entered; the watchdog waits against it
     rather than starting its own interval when it happens to be scheduled. */
  std::atomic<ULONGLONG> deadline_at( 0 );

  /* WatchdogGuard owns a thread that outlives this statement and reaches back
     into `scope`, the two event handles, and `deadline_at`. Locals are
     destroyed in reverse order and this guard's destructor joins that thread,
     so every one of those must stay declared ABOVE the guard. Locals declared
     below it are safe; the watchdog thread never touches them. Getting this
     wrong is silent: the event handles would close before
     ~WatchdogGuard signalled them, leaving its thread parked on an INFINITE
     wait and join() blocking until CI kills the step with no diagnostic. */
  WatchdogGuard watchdog( session_done.get(), deadline_start.get(), &deadline_at, &scope,
                          FAIRNESS_DEADLINE_MS );

  /* Establish the backlog synchronously before run() is entered. */
  const size_t minimum = ConsoleSession::input_budget_bytes() + 1;
  INPUT_RECORD records[RECORDS_PER_FILL_BATCH] = {};
  for ( INPUT_RECORD &record : records ) {
    record.EventType = KEY_EVENT;
    record.Event.KeyEvent.bKeyDown = TRUE;
    record.Event.KeyEvent.wRepeatCount = 1;
    record.Event.KeyEvent.uChar.AsciiChar = 'p';
  }
  const ULONGLONG fill_deadline = GetTickCount64() + FAIRNESS_FILL_TIMEOUT_MS;
  size_t batches = 0;
  DWORD last_written = 0;
  while ( session->input_backlog_for_test() < minimum ) {
    /* Report the batch count and the last accepted record count alongside the
       backlog: a console that accepts nothing and a reader thread that has
       stopped consuming both leave the backlog short, and only these tell them
       apart. */
    if ( GetTickCount64() >= fill_deadline ) {
      fprintf( stderr,
               "FAIL: could not establish input backlog within %lums "
               "(backlog=%zu minimum=%zu batches=%zu last_written=%lu)\n",
               FAIRNESS_FILL_TIMEOUT_MS, session->input_backlog_for_test(),
               minimum, batches, last_written );
      return 1;
    }
    /* The cap bounds how much this loop injects, not how long it waits. Records
       the console already accepted are still on their way to the reader, so
       past the cap keep polling until the deadline rather than calling a
       merely slow reader a setup failure. */
    if ( batches < FAIRNESS_FILL_MAX_BATCHES ) {
      if ( !WriteConsoleInputA( h_in, records, RECORDS_PER_FILL_BATCH,
                                &last_written ) ) {
        fprintf( stderr, "FAIL: WriteConsoleInputA failed (GetLastError=%lu)\n",
                 GetLastError() );
        return 1;
      }
      ++batches;
    }
    Sleep( 1 );
  }
  if ( !session->signal_termination_against_backlog_for_test( minimum ) ) {
    fprintf( stderr,
             "FAIL: input backlog fell below %zu at the atomic termination "
             "signal\n", minimum );
    return 1;
  }

  int thrown = 0;
  try {
    deadline_at.store( GetTickCount64() + FAIRNESS_DEADLINE_MS );
    must( SetEvent( deadline_start.get() ), "SetEvent(deadline_start)" );
    session->run();
  } catch ( const std::exception &e ) {
    fprintf( stderr, "FAIL: fairness: session.run() threw: %s\n", e.what() );
    thrown = 1;
  } catch ( ... ) {
    fprintf( stderr, "FAIL: fairness: session.run() threw a non-standard exception\n" );
    thrown = 1;
  }
  must( SetEvent( session_done.get() ), "SetEvent(session_done)" );
  watchdog.stop();

  /* The drain-at-shutdown flag records whether a drain had ever emptied the
     queue at the moment the loop first observed a shutdown request. Under the
     fair dispatch, termination wins a contested wakeup where the reader event
     is already signaled, so the drain that happens later in the same iteration
     will leave bytes in the queue. Under a winner-only dispatch, the reader
     event wins repeatedly, draining the queue to empty before termination is
     ever observed, so the sampled flag is true. */
  const bool shutdown_observed = session->shutdown_observed_for_test();
  const bool input_drained_at_shutdown = session->input_drained_at_shutdown_for_test();

  if ( thrown ) {
    return 1;
  }
  /* The loop must have observed termination and begun graceful shutdown. This
     is a direct observation of the thing we're testing, rather than inferring
     from the core's exit condition which could be confounded by connection
     timeout. */
  if ( !shutdown_observed ) {
    fprintf( stderr, "FAIL: session.run() did not observe termination\n" );
    return 1;
  }
  /* The discriminating assertion: on the wakeup where termination was first
     observed, the backlog had not yet drained to empty. If the reader event
     had won repeatedly (winner-only dispatch), the queue would be empty and
     termination would only be observed later on an idle wakeup. */
  if ( input_drained_at_shutdown ) {
    fprintf( stderr,
             "FAIL: reader backlog drained to empty before termination was observed; "
             "termination may have won an idle wakeup rather than a contested "
             "one\n" );
    return 1;
  }
  return 0;
}

/* Deadline for the clock-refresh harness to allow the transport shutdown
   handshake to finish. It is generous because it detects a wedge rather than
   measuring shutdown duration. */
static const DWORD CLOCK_REFRESH_DEADLINE_MS = 30000;
/* How long the loop is left running before termination is signaled. Each wait is
   capped at RESIZE_POLL_CAP_MS, so this window holds many iterations. */
static const DWORD CLOCK_REFRESH_RUN_MS = 1000;
/* Matches the recording capacity inside ConsoleSession; a short read is fine. */
static const size_t CLOCK_REFRESH_SAMPLE_CAPACITY = 64;

/* Ends run() from a second thread once the loop has had time to turn over
   several waits. It shares nothing with the test thread but the session pointer
   and a cancel event, and contributes nothing to the verdict, so it carries none
   of the attribution problems a thread that raced run() would. */
class TerminationTimerGuard {
public:
  TerminationTimerGuard( ConsoleSession *session, HANDLE cancel, DWORD delay_ms )
    : cancel_( cancel ),
      thread_( &TerminationTimerGuard::fire, session, cancel, delay_ms )
  {}
  ~TerminationTimerGuard()
  {
    stop();
  }
  void stop()
  {
    SetEvent( cancel_ );
    if ( thread_.joinable() ) {
      thread_.join();
    }
  }
  TerminationTimerGuard( const TerminationTimerGuard & ) = delete;
  TerminationTimerGuard &operator=( const TerminationTimerGuard & ) = delete;
private:
  static void fire( ConsoleSession *session, HANDLE cancel, DWORD delay_ms )
  {
    if ( WaitForSingleObject( cancel, delay_ms ) != WAIT_TIMEOUT ) {
      return;
    }
    /* The documented cross-thread entry point, which is also what a control
       handler would call, so this mode covers the real path rather than a
       test-only one that happens to signal the same event. */
    session->request_shutdown( ShutdownCause::CTRL_BREAK );
  }

  HANDLE cancel_;
  std::thread thread_;
};

/* Clock-refresh test: proves the event loop refreshes mosh's cached timestamp
   after its wait returns and before dispatching any source. Upstream gets this
   from Select::select(), which freezes the timestamp after pselect() returns; a
   loop that only freezes at the top of the iteration dispatches on a timestamp
   as old as the wait it just finished, backdating inbound packets by up to
   RESIZE_POLL_CAP_MS and understating RTT.

   Without the refresh, core.tick()'s freeze is the last thing to touch the clock
   before dispatch, so dispatch_ts equals tick_ts exactly on every iteration.
   Nothing between the two — socket reconciliation, the handle vector, the wait
   itself — freezes the timestamp. The assertion is therefore that some iteration
   advanced the clock at all, with no threshold and no reference to how long any
   wait ran: a duration test would measure the scheduler, and the defect it has
   to catch produces an exact zero rather than a small number.

   This mode establishes no input backlog and signals no termination up front, so
   the reader event stays quiet. Whether a wait then blocks long enough to cross
   a millisecond depends on the timeout mosh's transport asks for, which this
   harness does not control; if no iteration advances the clock the mode fails and
   names that outcome without claiming which cause produced it. */
static int run_clock_refresh()
{
  ConsoleTestScope scope;

  TestServer server( 80, 24 );
  MoshCore core( "127.0.0.1", server.port().c_str(), server.get_key().c_str(),
                 80, 24, never_prediction() );

  UniqueHandle session_done( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  if ( session_done.get() == NULL ) {
    fprintf( stderr, "fatal: CreateEvent(session_done) failed (GetLastError=%lu)\n",
             GetLastError() );
    return 1;
  }
  UniqueHandle deadline_start( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  if ( deadline_start.get() == NULL ) {
    fprintf( stderr, "fatal: CreateEvent(deadline_start) failed (GetLastError=%lu)\n",
             GetLastError() );
    return 1;
  }

  UniqueHandle timer_cancel( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  if ( timer_cancel.get() == NULL ) {
    fprintf( stderr, "fatal: CreateEvent(timer_cancel) failed (GetLastError=%lu)\n",
             GetLastError() );
    return 1;
  }

  auto session = std::make_unique<ConsoleSession>( core );
  std::atomic<ULONGLONG> deadline_at( 0 );

  /* Between them the two guards own threads that reach back into `scope`,
     `deadline_at`, and all three event handles (watchdog), and into `session`
     and `timer_cancel` (timer). Locals are destroyed in reverse order and both
     destructors join, so every one of those must stay declared ABOVE the guards.
     Locals declared below them are safe; neither thread touches those. */
  WatchdogGuard watchdog( session_done.get(), deadline_start.get(), &deadline_at, &scope,
                          CLOCK_REFRESH_DEADLINE_MS );
  TerminationTimerGuard timer( session.get(), timer_cancel.get(),
                               CLOCK_REFRESH_RUN_MS );

  int thrown = 0;
  try {
    deadline_at.store( GetTickCount64() + CLOCK_REFRESH_DEADLINE_MS );
    must( SetEvent( deadline_start.get() ), "SetEvent(deadline_start)" );
    session->run();
  } catch ( const std::exception &e ) {
    fprintf( stderr, "FAIL: clock-refresh: session.run() threw: %s\n", e.what() );
    thrown = 1;
  } catch ( ... ) {
    fprintf( stderr, "FAIL: clock-refresh: session.run() threw a non-standard exception\n" );
    thrown = 1;
  }
  must( SetEvent( session_done.get() ), "SetEvent(session_done)" );
  timer.stop();
  watchdog.stop();

  if ( thrown ) {
    return 1;
  }

  ConsoleSession::ClockRefreshSample samples[CLOCK_REFRESH_SAMPLE_CAPACITY];
  const size_t count =
    session->clock_refresh_samples_for_test( samples, CLOCK_REFRESH_SAMPLE_CAPACITY );
  if ( count == 0 ) {
    fprintf( stderr, "FAIL: run() recorded no event-loop iterations\n" );
    return 1;
  }

  uint64_t widest_advance = 0;
  for ( size_t i = 0; i < count; ++i ) {
    const ConsoleSession::ClockRefreshSample &sample = samples[i];
    if ( sample.dispatch_ts < sample.tick_ts ) {
      fprintf( stderr,
               "FAIL: iteration %zu ran the cached timestamp backwards "
               "(tick_ts=%llu dispatch_ts=%llu)\n",
               i, (unsigned long long)sample.tick_ts,
               (unsigned long long)sample.dispatch_ts );
      return 1;
    }
    const uint64_t advance = sample.dispatch_ts - sample.tick_ts;
    if ( advance > widest_advance ) {
      widest_advance = advance;
    }
  }

  if ( widest_advance == 0 ) {
    fprintf( stderr,
             "FAIL: none of %zu event-loop iterations advanced the cached "
             "timestamp across the wait; every source was dispatched on the "
             "timestamp core.tick() froze before it\n",
             count );
    return 1;
  }
  return 0;
}

/* How setup is made to stop at a given step. INJECT_FAILURE takes the injected
   branch of the checkpoint; SIGNAL_TERMINATION sets the real termination event
   and lets setup stop at the same zero-timeout wait production uses, so that
   branch cannot be deleted without failing a test. */
enum class RollbackTrigger { INJECT_FAILURE, SIGNAL_TERMINATION };

/* Arms `trigger` for `step`, constructs a ConsoleSession expecting the
   constructor to roll back and throw, then checks the console against its
   pre-construction state. Proves the named step's checkpoint fires, that every
   mutation applied before it is undone, and that restoration was announced. */
static int run_rollback_case( ConsoleSetupStep step, RollbackTrigger trigger,
                              const char *label )
{
  ConsoleTestScope scope;

  HANDLE h_out = GetStdHandle( STD_OUTPUT_HANDLE );
  HANDLE h_in = GetStdHandle( STD_INPUT_HANDLE );
  DWORD before_out_mode = 0;
  DWORD before_in_mode = 0;
  must( GetConsoleMode( h_out, &before_out_mode ), "GetConsoleMode(h_out, before)" );
  must( GetConsoleMode( h_in, &before_in_mode ), "GetConsoleMode(h_in, before)" );
  const UINT before_out_cp = GetConsoleOutputCP();
  const UINT before_in_cp = GetConsoleCP();

  TestServer server( 80, 24 );
  MoshCore core( "127.0.0.1", server.port().c_str(), server.get_key().c_str(),
                 80, 24, never_prediction() );

  console_test_clear_setup_injections();
  const DWORD expected_code = trigger == RollbackTrigger::INJECT_FAILURE
    ? ERROR_CANCELLED : ERROR_OPERATION_ABORTED;
  if ( trigger == RollbackTrigger::INJECT_FAILURE ) {
    console_test_fail_after( step );
  } else {
    console_test_signal_termination_after( step );
  }

  bool threw = false;
  DWORD actual_code = ERROR_SUCCESS;
  try {
    ConsoleSession session( core );
  } catch ( const ConsoleError &error ) {
    threw = true;
    actual_code = error.win32_code;
  } catch ( const std::exception &e ) {
    console_test_clear_setup_injections();
    fprintf( stderr, "FAIL: %s: constructor threw the wrong exception type: %s\n",
             label, e.what() );
    return 1;
  }
  console_test_clear_setup_injections();
  if ( !threw ) {
    fprintf( stderr, "FAIL: %s: constructor did not stop at the checkpoint\n", label );
    return 1;
  }
  /* The code distinguishes which branch of the checkpoint stopped setup, so a
     termination case cannot pass by taking the injected-failure path. */
  if ( actual_code != expected_code ) {
    fprintf( stderr, "FAIL: %s: stopped with error %lu, expected %lu\n",
             label, actual_code, expected_code );
    return 1;
  }
  /* No session survives a throwing constructor, so the restored event is read
     from the control block, which outlives it deliberately. */
  const HANDLE restored = console_test_last_restored_event();
  if ( restored == NULL || WaitForSingleObject( restored, 0 ) != WAIT_OBJECT_0 ) {
    fprintf( stderr, "FAIL: %s: rollback did not signal restored\n", label );
    return 1;
  }

  DWORD after_out_mode = 0;
  DWORD after_in_mode = 0;
  must( GetConsoleMode( h_out, &after_out_mode ), "GetConsoleMode(h_out, after)" );
  must( GetConsoleMode( h_in, &after_in_mode ), "GetConsoleMode(h_in, after)" );
  const UINT after_out_cp = GetConsoleOutputCP();
  const UINT after_in_cp = GetConsoleCP();
  if ( after_out_mode != before_out_mode || after_in_mode != before_in_mode
       || after_out_cp != before_out_cp || after_in_cp != before_in_cp ) {
    fprintf( stderr, "FAIL: %s: console state not fully rolled back\n", label );
    return 1;
  }
  return 0;
}

/* Fails after INPUT_MODE (one mutation to unwind) and after INPUT_CODE_PAGE
   (three: input mode, output mode, input code page), proving rollback chains
   of different depths all unwind completely rather than just the single-step
   case. */
static int run_transaction()
{
  if ( run_rollback_case( ConsoleSetupStep::INPUT_MODE,
                          RollbackTrigger::INJECT_FAILURE, "transaction/input-mode" ) != 0 ) {
    return 1;
  }
  if ( run_rollback_case( ConsoleSetupStep::INPUT_CODE_PAGE,
                          RollbackTrigger::INJECT_FAILURE, "transaction/input-code-page" ) != 0 ) {
    return 1;
  }
  return 0;
}

/* Publishes a real shutdown between setup mutations, once after each of the
   four, and proves setup stops at the production termination checkpoint, undoes
   every mutation applied so far, and signals restored. Unlike transaction this
   goes through no injected failure: the checkpoint's zero-timeout wait on the
   termination event is what has to fire. */
static int run_init_checkpoint()
{
  static const ConsoleSetupStep steps[] = {
    ConsoleSetupStep::INPUT_MODE,
    ConsoleSetupStep::OUTPUT_MODE,
    ConsoleSetupStep::INPUT_CODE_PAGE,
    ConsoleSetupStep::OUTPUT_CODE_PAGE,
  };
  static const char *labels[] = {
    "init-checkpoint/input-mode", "init-checkpoint/output-mode",
    "init-checkpoint/input-code-page", "init-checkpoint/output-code-page",
  };
  for ( size_t i = 0; i < sizeof( steps ) / sizeof( steps[0] ); ++i ) {
    if ( run_rollback_case( steps[i], RollbackTrigger::SIGNAL_TERMINATION,
                            labels[i] ) != 0 ) {
      return 1;
    }
  }
  return 0;
}

/* Sixteen unacknowledged shutdown sends are spaced by the 250 ms transport
   interval plus at most the 100 ms console poll cap, so healthy completion is
   expected in 4--5.6 s. These acceptance watchdogs allow more than five times
   that ceiling; they diagnose wedges rather than measure transport timing. */
static const DWORD ACCEPTANCE_WATCHDOG_MS = 30000;
/* Delay before signaling termination, to ensure run() has entered its loop.
   The loop must be pumping before the request can be observed and the graceful
   shutdown initiated. */
static const DWORD GRACEFUL_SHUTDOWN_SIGNAL_DELAY_MS = 100;

static int run_reader_end( ConsoleReaderTestOutcome outcome, const char *label )
{
  ConsoleTestScope scope;
  TestServer server( 80, 24 );
  MoshCore core( "127.0.0.1", server.port().c_str(), server.get_key().c_str(),
                 80, 24, never_prediction() );
  console_test_set_reader_outcome( outcome );

  UniqueHandle session_done( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  must( session_done.get() != NULL, "CreateEvent(session_done)" );
  UniqueHandle deadline_start( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  must( deadline_start.get() != NULL, "CreateEvent(deadline_start)" );
  std::atomic<ULONGLONG> deadline_at( 0 );
  WatchdogGuard watchdog( session_done.get(), deadline_start.get(), &deadline_at, &scope,
                          ACCEPTANCE_WATCHDOG_MS );

  ConsoleSession session( core );
  try {
    deadline_at.store( GetTickCount64() + ACCEPTANCE_WATCHDOG_MS );
    must( SetEvent( deadline_start.get() ), "SetEvent(deadline_start)" );
    session.run();
  } catch ( const std::exception &e ) {
    must( SetEvent( session_done.get() ), "SetEvent(session_done)" );
    watchdog.stop();
    console_test_clear_setup_injections();
    fprintf( stderr, "FAIL: %s: session.run() threw: %s\n", label, e.what() );
    return 1;
  }
  must( SetEvent( session_done.get() ), "SetEvent(session_done)" );
  watchdog.stop();
  console_test_clear_setup_injections();
  if ( !session.shutdown_observed_for_test() || !core.is_finished()
       || core.exited_cleanly() ) {
    fprintf( stderr, "FAIL: %s did not complete the unacknowledged graceful path\n", label );
    return 1;
  }
  const CleanupReport cleanup = session.cleanup_report();
  const DWORD expected_reader_error = outcome == ConsoleReaderTestOutcome::READ_FAILURE
    ? ERROR_READ_FAULT : ERROR_SUCCESS;
  if ( cleanup.reader_error != expected_reader_error ) {
    fprintf( stderr, "FAIL: %s reported reader error %lu, expected %lu\n",
             label, cleanup.reader_error, expected_reader_error );
    return 1;
  }
  return 0;
}

/* A 900 ms injected OS budget leaves 400 ms of pumping before the 500 ms
   restoration reserve. That reserve break precedes the transport's earliest
   4000 ms retry-cap completion by 3100 ms, making deadline exit deterministic. */
static const DWORD INJECTED_SHUTDOWN_BUDGET_MS = 900;
static const DWORD DEADLINE_STAMP_TOLERANCE_MS = 100;
static const DWORD DEADLINE_COMPLETION_FLOOR_MS = 300;
static const DWORD DELAYED_OBSERVATION_MS = 200;
/* A close is published while reader teardown is in one 100 ms wait. Ten such
   poll intervals allow scheduling and restoration headroom while still proving
   that the close diverts teardown promptly; this bound is independent of the
   transport and OS shutdown budgets. */
static const DWORD MID_TEARDOWN_CLOSE_BOUND_MS = 1000;

class ConsoleTestInjectionGuard {
public:
  ConsoleTestInjectionGuard() {}
  ~ConsoleTestInjectionGuard()
  {
    console_test_clear_setup_injections();
  }
  ConsoleTestInjectionGuard( const ConsoleTestInjectionGuard & ) = delete;
  ConsoleTestInjectionGuard &operator=( const ConsoleTestInjectionGuard & ) = delete;
};

class ShutdownRequestGuard {
public:
  ShutdownRequestGuard( ConsoleSession *session, std::atomic<ULONGLONG> *close_publication )
    : thread_( &ShutdownRequestGuard::fire, session, close_publication ) {}
  ~ShutdownRequestGuard()
  {
    if ( thread_.joinable() ) {
      thread_.join();
    }
  }
private:
  static void fire( ConsoleSession *session, std::atomic<ULONGLONG> *close_publication )
  {
    Sleep( 100 );
    session->request_shutdown( ShutdownCause::CTRL_BREAK );
    Sleep( 200 );
    close_publication->store( GetTickCount64() );
    session->request_shutdown( ShutdownCause::CTRL_CLOSE );
  }
  std::thread thread_;
};

class MidTeardownCloseGuard {
public:
  MidTeardownCloseGuard( ConsoleSession *session, HANDLE teardown_entered,
                         HANDLE teardown_resume,
                         std::atomic<ULONGLONG> *close_publication )
    : thread_( &MidTeardownCloseGuard::fire, session, teardown_entered,
               teardown_resume, close_publication ) {}
  ~MidTeardownCloseGuard()
  {
    if ( thread_.joinable() ) {
      thread_.join();
    }
  }
private:
  static void fire( ConsoleSession *session, HANDLE teardown_entered,
                    HANDLE teardown_resume,
                    std::atomic<ULONGLONG> *close_publication )
  {
    if ( WaitForSingleObject( teardown_entered, ACCEPTANCE_WATCHDOG_MS ) != WAIT_OBJECT_0 ) {
      return;
    }
    close_publication->store( GetTickCount64() );
    session->request_shutdown( ShutdownCause::CTRL_CLOSE );
    SetEvent( teardown_resume );
  }
  std::thread thread_;
};

enum class DeadlineCase {
  PUBLICATION,
  ESCALATION,
};

static int run_deadline_case( DeadlineCase which )
{
  ConsoleTestInjectionGuard injections;
  ConsoleTestScope scope;
  TestServer server( 80, 24 );
  MoshCore core( "127.0.0.1", server.port().c_str(), server.get_key().c_str(),
                 80, 24, never_prediction() );
  console_test_set_shutdown_budget( INJECTED_SHUTDOWN_BUDGET_MS );
  UniqueHandle session_done( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  must( session_done.get() != NULL, "CreateEvent(session_done)" );
  UniqueHandle deadline_start( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  must( deadline_start.get() != NULL, "CreateEvent(deadline_start)" );
  std::atomic<ULONGLONG> watchdog_at( 0 );
  WatchdogGuard watchdog( session_done.get(), deadline_start.get(), &watchdog_at, &scope,
                          ACCEPTANCE_WATCHDOG_MS );

  auto session = std::make_unique<ConsoleSession>( core );
  std::atomic<ULONGLONG> close_publication( 0 );
  std::unique_ptr<ShutdownRequestGuard> requests;
  if ( which == DeadlineCase::ESCALATION ) {
    requests.reset( new ShutdownRequestGuard( session.get(), &close_publication ) );
  } else {
    close_publication.store( GetTickCount64() );
    session->request_shutdown( ShutdownCause::CTRL_CLOSE );
    Sleep( DELAYED_OBSERVATION_MS );
  }

  watchdog_at.store( GetTickCount64() + ACCEPTANCE_WATCHDOG_MS );
  must( SetEvent( deadline_start.get() ), "SetEvent(deadline_start)" );
  session->run();
  const ULONGLONG completed = GetTickCount64();
  must( SetEvent( session_done.get() ), "SetEvent(session_done)" );
  watchdog.stop();

  const ULONGLONG publication = close_publication.load();
  const ULONGLONG expected_deadline = publication + INJECTED_SHUTDOWN_BUDGET_MS;
  const ULONGLONG adopted_deadline = session->termination_deadline_for_test();
  if ( publication == 0 || !session->shutdown_observed_for_test() ) {
    fprintf( stderr, "FAIL: deadline request was not observed and published\n" );
    return 1;
  }
  /* With a 900 ms budget and 500 ms reserve, correct pumping ends about
     400 ms after publication. The 300 ms floor leaves one 100 ms poll interval
     of scheduling margin while rejecting an immediate stopgap break. */
  if ( completed < publication + DEADLINE_COMPLETION_FLOOR_MS
       || completed > expected_deadline ) {
    fprintf( stderr,
             "FAIL: deadline shutdown completed outside its external %lu--%lu ms window\n",
             DEADLINE_COMPLETION_FLOOR_MS, INJECTED_SHUTDOWN_BUDGET_MS );
    return 1;
  }
  if ( adopted_deadline < expected_deadline
       || adopted_deadline > expected_deadline + DEADLINE_STAMP_TOLERANCE_MS ) {
    fprintf( stderr,
             "FAIL: adopted deadline %llu is not the close publication budget %llu--%llu\n",
             (unsigned long long)adopted_deadline,
             (unsigned long long)expected_deadline,
             (unsigned long long)( expected_deadline + DEADLINE_STAMP_TOLERANCE_MS ) );
    return 1;
  }
  DWORD restored_mode = 0;
  const ConsoleSnapshot snapshot = session->original_console_for_test();
  must( GetConsoleMode( snapshot.input, &restored_mode ), "GetConsoleMode(deadline restored)" );
  if ( restored_mode != snapshot.input_mode ) {
    fprintf( stderr, "FAIL: deadline shutdown returned before restoring input mode\n" );
    return 1;
  }
  return 0;
}

static int run_mid_teardown_close()
{
  ConsoleTestInjectionGuard injections;
  ConsoleTestScope scope;
  TestServer server( 80, 24 );
  MoshCore core( "127.0.0.1", server.port().c_str(), server.get_key().c_str(),
                 80, 24, never_prediction() );
  console_test_set_reader_outcome( ConsoleReaderTestOutcome::NONTERMINATING );

  UniqueHandle session_done( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  must( session_done.get() != NULL, "CreateEvent(session_done)" );
  UniqueHandle deadline_start( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  must( deadline_start.get() != NULL, "CreateEvent(deadline_start)" );
  UniqueHandle teardown_entered( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  must( teardown_entered.get() != NULL, "CreateEvent(teardown_entered)" );
  UniqueHandle teardown_resume( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  must( teardown_resume.get() != NULL, "CreateEvent(teardown_resume)" );
  std::atomic<ULONGLONG> watchdog_at( 0 );
  WatchdogGuard watchdog( session_done.get(), deadline_start.get(), &watchdog_at, &scope,
                          ACCEPTANCE_WATCHDOG_MS );

  auto session = std::make_unique<ConsoleSession>( core );
  console_test_pause_teardown( teardown_entered.get(), teardown_resume.get() );
  std::atomic<ULONGLONG> close_publication( 0 );
  MidTeardownCloseGuard request( session.get(), teardown_entered.get(),
                                 teardown_resume.get(), &close_publication );
  const char quit[] = { 0x1e, '.' };
  core.feed_input( quit, sizeof quit );
  watchdog_at.store( GetTickCount64() + ACCEPTANCE_WATCHDOG_MS );
  must( SetEvent( deadline_start.get() ), "SetEvent(deadline_start)" );
  session->run();
  const ULONGLONG completed = GetTickCount64();
  must( SetEvent( session_done.get() ), "SetEvent(session_done)" );
  watchdog.stop();

  int failed = 0;
  if ( close_publication.load() == 0
       || completed > close_publication.load() + MID_TEARDOWN_CLOSE_BOUND_MS ) {
    fprintf( stderr,
             "FAIL: close during reader teardown did not divert restoration promptly\n" );
    failed = 1;
  }
  DWORD restored_mode = 0;
  const ConsoleSnapshot snapshot = session->original_console_for_test();
  must( GetConsoleMode( snapshot.input, &restored_mode ),
        "GetConsoleMode(mid-teardown restored)" );
  if ( restored_mode != snapshot.input_mode ) {
    fprintf( stderr, "FAIL: close during teardown did not restore input mode\n" );
    failed = 1;
  }
  session.release();
  return failed;
}

static int run_connection_timeout()
{
  ConsoleTestScope scope;
  TestServer server( 80, 24 );
  const ULONGLONG started = GetTickCount64();
  MoshCore core( "127.0.0.1", server.port().c_str(), server.get_key().c_str(),
                 80, 24, never_prediction() );

  UniqueHandle session_done( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  must( session_done.get() != NULL, "CreateEvent(session_done)" );
  UniqueHandle deadline_start( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  must( deadline_start.get() != NULL, "CreateEvent(deadline_start)" );
  std::atomic<ULONGLONG> deadline_at( 0 );
  WatchdogGuard watchdog( session_done.get(), deadline_start.get(), &deadline_at, &scope,
                          ACCEPTANCE_WATCHDOG_MS );

  ConsoleSession session( core );
  deadline_at.store( started + ACCEPTANCE_WATCHDOG_MS );
  must( SetEvent( deadline_start.get() ), "SetEvent(deadline_start)" );
  session.run();
  const ULONGLONG elapsed = GetTickCount64() - started;
  must( SetEvent( session_done.get() ), "SetEvent(session_done)" );
  watchdog.stop();

  /* The 15 s connection timeout plus the 4--5.6 s retry budget predicts
     19--20.6 s. The 18--23 s window leaves 1 s below the arithmetic floor,
     2.4 s above its ceiling, and makes the old immediate 15 s exit fail. */
  if ( elapsed < 18000 || elapsed > 23000 || !core.is_finished()
       || core.exited_cleanly()
       || core.status_message() != "Timed out waiting for server..." ) {
    fprintf( stderr,
             "FAIL: connection timeout did not use transport shutdown (elapsed=%llu ms)\n",
             (unsigned long long)elapsed );
    return 1;
  }
  return 0;
}

static int run_upstream_length()
{
  ConsoleTestScope scope;
  TestServer server( 80, 24 );
  MoshCore core( "127.0.0.1", server.port().c_str(), server.get_key().c_str(),
                 80, 24, never_prediction() );

  UniqueHandle session_done( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  must( session_done.get() != NULL, "CreateEvent(session_done)" );
  UniqueHandle deadline_start( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  must( deadline_start.get() != NULL, "CreateEvent(deadline_start)" );
  std::atomic<ULONGLONG> deadline_at( 0 );
  WatchdogGuard watchdog( session_done.get(), deadline_start.get(), &deadline_at, &scope,
                          ACCEPTANCE_WATCHDOG_MS );

  ConsoleSession session( core );
  const char quit[] = { 0x1e, '.' };
  core.feed_input( quit, sizeof quit );
  const ULONGLONG started = GetTickCount64();
  deadline_at.store( started + ACCEPTANCE_WATCHDOG_MS );
  must( SetEvent( deadline_start.get() ), "SetEvent(deadline_start)" );
  session.run();
  const ULONGLONG elapsed = GetTickCount64() - started;
  must( SetEvent( session_done.get() ), "SetEvent(session_done)" );
  watchdog.stop();

  /* Sixteen retries at 250--350 ms predict 4--5.6 s. The 3--8 s window is
     comfortably above a one-retry exit, leaves 1 s below the arithmetic floor
     and 2.4 s above its ceiling, yet stays 2 s below the 10 s timeout ceiling. */
  if ( session.termination_deadline_for_test() != 0
       || elapsed < 3000 || elapsed > 8000 || !core.is_finished()
       || core.exited_cleanly() ) {
    fprintf( stderr,
             "FAIL: uncapped in-band shutdown did not use transport bound "
             "(elapsed=%llu ms deadline=%llu)\n",
             (unsigned long long)elapsed,
             (unsigned long long)session.termination_deadline_for_test() );
    return 1;
  }
  return 0;
}

static int run_graceful_shutdown()
{
  ConsoleTestScope scope;

  TestServer server( 80, 24 );
  MoshCore core( "127.0.0.1", server.port().c_str(), server.get_key().c_str(),
                 80, 24, never_prediction() );

  UniqueHandle session_done( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  if ( session_done.get() == NULL ) {
    fprintf( stderr, "fatal: CreateEvent(session_done) failed (GetLastError=%lu)\n",
             GetLastError() );
    return 1;
  }
  UniqueHandle deadline_start( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  if ( deadline_start.get() == NULL ) {
    fprintf( stderr, "fatal: CreateEvent(deadline_start) failed (GetLastError=%lu)\n",
             GetLastError() );
    return 1;
  }

  auto session = std::make_unique<ConsoleSession>( core );
  std::atomic<ULONGLONG> deadline_at( 0 );

  WatchdogGuard watchdog( session_done.get(), deadline_start.get(), &deadline_at, &scope,
                          ACCEPTANCE_WATCHDOG_MS );

  /* Signal the shutdown request after a short delay to ensure run() has started. */
  TerminationTimerGuard signaler( session.get(), session_done.get(),
                                  GRACEFUL_SHUTDOWN_SIGNAL_DELAY_MS );

  int thrown = 0;
  try {
    deadline_at.store( GetTickCount64() + ACCEPTANCE_WATCHDOG_MS );
    must( SetEvent( deadline_start.get() ), "SetEvent(deadline_start)" );
    session->run();
  } catch ( const std::exception &e ) {
    fprintf( stderr, "FAIL: graceful-shutdown: session.run() threw: %s\n", e.what() );
    thrown = 1;
  } catch ( ... ) {
    fprintf( stderr, "FAIL: graceful-shutdown: session.run() threw a non-standard exception\n" );
    thrown = 1;
  }
  signaler.stop();
  must( SetEvent( session_done.get() ), "SetEvent(session_done)" );
  watchdog.stop();

  if ( thrown ) {
    return 1;
  }

  /* The loop must have observed termination and begun graceful shutdown. */
  if ( !session->shutdown_observed_for_test() ) {
    fprintf( stderr, "FAIL: session.run() did not observe termination\n" );
    return 1;
  }

  /* The loop must have exited through core.is_finished(), not a break. */
  if ( !core.is_finished() ) {
    fprintf( stderr, "FAIL: session.run() did not exit through core.is_finished()\n" );
    return 1;
  }

  /* The status message should be "Exiting..." which is set by begin_shutdown().
     This proves the session went through the graceful shutdown path. */
  const std::string &status = core.status_message();
  if ( status != "Exiting..." ) {
    fprintf( stderr,
             "FAIL: status message was \"%s\", expected \"Exiting...\"\n",
             status.c_str() );
    return 1;
  }

  /* The clean_shutdown flag is set after either shutdown acknowledgement has
     been received or the counterparty's acknowledgement has been sent. It stays
     false on the timeout path, proving this session used that path. */
  if ( core.exited_cleanly() ) {
    fprintf( stderr, "FAIL: session exited cleanly (expected timeout path)\n" );
    return 1;
  }

  return 0;
}

/* Proves the PollThrottle arithmetic that keeps run_loop() from spinning when a
   source keeps requesting a zero wait. Constructs PollThrottle objects directly
   and never touches a ConsoleSession or the event loop. */
static int run_poll_throttle()
{
  /* Case 1: a nonzero request passes through unchanged, even when repeated past the
     boundary count. */
  {
    PollThrottle throttle;
    DWORD got = throttle.bound( 100 );
    if ( got != 100 ) {
      fprintf( stderr, "FAIL: poll-throttle: nonzero request returned %lu, expected 100\n",
               got );
      return 1;
    }
    for ( unsigned i = 0; i < PollThrottle::MAX_CONSECUTIVE_POLLS + 1; ++i ) {
      got = throttle.bound( 100 );
      if ( got != 100 ) {
        fprintf( stderr, "FAIL: poll-throttle: repeated nonzero request returned %lu on call %u, expected 100\n",
                 got, i + 2 );
        return 1;
      }
    }
  }

  /* Case 2: the first nine consecutive zero requests each return 0. */
  {
    PollThrottle throttle;
    for ( unsigned i = 0; i < PollThrottle::MAX_CONSECUTIVE_POLLS - 1; ++i ) {
      DWORD got = throttle.bound( 0 );
      if ( got != 0 ) {
        fprintf( stderr, "FAIL: poll-throttle: early zero request returned %lu on call %u, expected 0\n",
                 got, i + 1 );
        return 1;
      }
    }
  }

  /* Case 3: the tenth consecutive zero returns 1, and every further zero also
     returns 1. Driven well past the boundary to catch a counter that wraps or
     resets itself and hands out another burst of zero waits. */
  {
    PollThrottle throttle;
    DWORD got = throttle.bound( 0 );
    for ( unsigned i = 1; i < PollThrottle::MAX_CONSECUTIVE_POLLS; ++i ) {
      got = throttle.bound( 0 );
    }
    if ( got != 1 ) {
      fprintf( stderr, "FAIL: poll-throttle: tenth consecutive zero returned %lu, expected 1\n",
               got );
      return 1;
    }
    for ( unsigned i = 0; i < 10000; ++i ) {
      got = throttle.bound( 0 );
      if ( got != 1 ) {
        fprintf( stderr, "FAIL: poll-throttle: sustained zero returned %lu on call %u after the floor engaged, expected 1\n",
                 got, i + 1 );
        return 1;
      }
    }
  }

  /* Case 4: a nonzero request clears the count. Nine zeros, then a nonzero
     request passes through and resets, then the next nine zeros each return 0
     and the tenth returns 1. */
  {
    PollThrottle throttle;
    for ( unsigned i = 0; i < PollThrottle::MAX_CONSECUTIVE_POLLS - 1; ++i ) {
      DWORD got = throttle.bound( 0 );
      if ( got != 0 ) {
        fprintf( stderr, "FAIL: poll-throttle: pre-clear zero returned %lu on call %u, expected 0\n",
                 got, i + 1 );
        return 1;
      }
    }
    DWORD got = throttle.bound( 100 );
    if ( got != 100 ) {
      fprintf( stderr, "FAIL: poll-throttle: clearing nonzero request returned %lu, expected 100\n",
               got );
      return 1;
    }
    for ( unsigned i = 0; i < PollThrottle::MAX_CONSECUTIVE_POLLS - 1; ++i ) {
      got = throttle.bound( 0 );
      if ( got != 0 ) {
        fprintf( stderr, "FAIL: poll-throttle: post-clear zero returned %lu on call %u, expected 0\n",
                 got, i + 1 );
        return 1;
      }
    }
    got = throttle.bound( 0 );
    if ( got != 1 ) {
      fprintf( stderr, "FAIL: poll-throttle: post-clear tenth zero returned %lu, expected 1\n",
               got );
      return 1;
    }
  }

  /* Case 5: a nonzero request clears the count after the floor has engaged.
     Driven well past the boundary so the floor is engaged, then a nonzero
     request passes through, then the next zero returns 0 — not 1. */
  {
    PollThrottle throttle;
    for ( unsigned i = 0; i < 2 * PollThrottle::MAX_CONSECUTIVE_POLLS; ++i ) {
      throttle.bound( 0 );
    }
    DWORD got = throttle.bound( 100 );
    if ( got != 100 ) {
      fprintf( stderr, "FAIL: poll-throttle: floor-clearing nonzero request returned %lu, expected 100\n",
               got );
      return 1;
    }
    got = throttle.bound( 0 );
    if ( got != 0 ) {
      fprintf( stderr, "FAIL: poll-throttle: first zero after floor cleared returned %lu, expected 0\n",
               got );
      return 1;
    }
  }

  return 0;
}

/* ---------------------------------------------------------------------------
   Ctrl-Z console-read behavior: probe and end-to-end passthrough test.

   On a console host older than microsoft/terminal PR #19940, a raw-mode
   ReadFile on the console input handle reports Ctrl-Z as a successful
   ZERO-BYTE read and consumes the 0x1A byte; on a fixed host the same
   keystroke arrives as a literal 0x1A. probe-ctrlz-read records which
   behavior the attached host exhibits; ctrl-z-passthrough asserts the client
   forwards 0x1A to the server either way (using the reader injector to
   simulate the old host); probe-ctrlz-e2e runs the same assertion against a
   physically injected Ctrl-Z on the real host. */

/* Types one keystroke (down + up) into the console input buffer. */
static void inject_key( HANDLE h_in, WORD vk, DWORD control_state, char ascii )
{
  INPUT_RECORD records[2] = {};
  for ( INPUT_RECORD &record : records ) {
    record.EventType = KEY_EVENT;
    record.Event.KeyEvent.wVirtualKeyCode = vk;
    record.Event.KeyEvent.wRepeatCount = 1;
    record.Event.KeyEvent.dwControlKeyState = control_state;
    record.Event.KeyEvent.uChar.AsciiChar = ascii;
  }
  records[0].Event.KeyEvent.bKeyDown = TRUE;
  DWORD written = 0;
  must( WriteConsoleInputA( h_in, records, 2, &written ) && written == 2,
        "WriteConsoleInputA(inject_key)" );
}

/* Raw-API ground truth for the attached console host. Applies mosh's exact
   input mode, injects a physical Ctrl-Z, and reports what ReadFile returns.
   Read 2 runs with no further input pending: a kicker thread types 'q' after
   500 ms so a blocking read completes. An immediate zero-byte read 2 would
   mean the EOF state latches, which the per-keystroke synthesis fix could not
   safely paper over. Always returns 0: it records, it does not assert. */
static int run_probe_ctrlz_read()
{
  ConsoleTestScope scope;
  HANDLE h_in = GetStdHandle( STD_INPUT_HANDLE );

  DWORD mode = 0;
  must( GetConsoleMode( h_in, &mode ), "GetConsoleMode(probe)" );
  mode = ( mode | ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS )
    & ~( ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE );
  must( SetConsoleMode( h_in, mode ), "SetConsoleMode(probe raw)" );
  must( FlushConsoleInputBuffer( h_in ), "FlushConsoleInputBuffer(probe)" );

  inject_key( h_in, 'Z', LEFT_CTRL_PRESSED, 0x1a );

  char buf[64];
  DWORD read = 0;
  const ULONGLONG read1_start = GetTickCount64();
  const BOOL ok1 = ReadFile( h_in, buf, sizeof buf, &read, NULL );
  const DWORD err1 = ok1 ? ERROR_SUCCESS : GetLastError();
  fprintf( stderr, "PROBE read1: ok=%d bytes=%lu err=%lu elapsed_ms=%llu",
           ok1 ? 1 : 0, read, err1,
           (unsigned long long)( GetTickCount64() - read1_start ) );
  if ( ok1 && read > 0 ) {
    fprintf( stderr, " byte0=0x%02x", (unsigned char)buf[0] );
  }
  fprintf( stderr, "\n" );

  std::atomic<bool> read2_done( false );
  std::thread kicker( [&]() {
    Sleep( 500 );
    if ( !read2_done.load() ) {
      inject_key( h_in, 'Q', 0, 'q' );
    }
  } );
  char buf2[64];
  DWORD read2n = 0;
  const ULONGLONG read2_start = GetTickCount64();
  const BOOL ok2 = ReadFile( h_in, buf2, sizeof buf2, &read2n, NULL );
  const DWORD err2 = ok2 ? ERROR_SUCCESS : GetLastError();
  const ULONGLONG read2_ms = GetTickCount64() - read2_start;
  read2_done.store( true );
  kicker.join();
  fprintf( stderr, "PROBE read2: ok=%d bytes=%lu err=%lu elapsed_ms=%llu",
           ok2 ? 1 : 0, read2n, err2, (unsigned long long)read2_ms );
  if ( ok2 && read2n > 0 ) {
    fprintf( stderr, " byte0=0x%02x", (unsigned char)buf2[0] );
  }
  fprintf( stderr, "\n" );
  return 0;
}

/* End-to-end Ctrl-Z passthrough. A console session fed a Ctrl-Z must forward
   0x1A to the server — the TestServer's echo writeback lands it in cell
   (0,0) — and must not begin shutdown on its own. With use_injection the
   zero-byte read comes from the reader test injector (simulated old conhost);
   without it a physical Ctrl-Z is typed into the real console and the
   attached host's own behavior applies, so the mode doubles as the probe of
   whether the host needs the compatibility path at all. */
static int run_ctrlz_passthrough( bool use_injection )
{
  const char *label = use_injection ? "ctrl-z-passthrough" : "probe-ctrlz-e2e";
  ConsoleTestScope scope;
  TestServer server( 80, 24 );
  MoshCore core( "127.0.0.1", server.port().c_str(), server.get_key().c_str(),
                 80, 24, never_prediction() );
  if ( use_injection ) {
    console_test_set_reader_outcome( ConsoleReaderTestOutcome::ZERO_BYTE_READ );
  }

  UniqueHandle session_done( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  must( session_done.get() != NULL, "CreateEvent(session_done)" );
  UniqueHandle deadline_start( CreateEvent( NULL, TRUE, FALSE, NULL ) );
  must( deadline_start.get() != NULL, "CreateEvent(deadline_start)" );
  std::atomic<ULONGLONG> deadline_at( 0 );

  ConsoleSession session( core );
  WatchdogGuard watchdog( session_done.get(), deadline_start.get(), &deadline_at, &scope,
                          ACCEPTANCE_WATCHDOG_MS );

  std::atomic<bool> stop_pumping( false );
  std::thread runner( [&]() {
    deadline_at.store( GetTickCount64() + ACCEPTANCE_WATCHDOG_MS );
    must( SetEvent( deadline_start.get() ), "SetEvent(deadline_start)" );
    session.run();
  } );
  std::thread pump( [&]() {
    while ( !stop_pumping.load() ) {
      for ( const intptr_t fd : server.socket_fds() ) {
        server.on_readable( fd );
      }
      server.tick();
      Sleep( 10 );
    }
  } );

  if ( !use_injection ) {
    /* Let run() reach its first wait, then type a physical Ctrl-Z. */
    Sleep( 250 );
    inject_key( GetStdHandle( STD_INPUT_HANDLE ), 'Z', LEFT_CTRL_PRESSED, 0x1a );
  }

  const ULONGLONG echo_deadline = GetTickCount64() + 5000;
  bool echoed = false;
  while ( GetTickCount64() < echo_deadline ) {
    if ( server.cell_contents_is( 0, 0, "\x1a", 1 ) ) {
      echoed = true;
      break;
    }
    Sleep( 20 );
  }
  /* Sampled before our own shutdown request: if the Ctrl-Z already began a
     shutdown (the bug), this is true and the echo above never arrived. */
  const bool shutdown_before_request = session.shutdown_observed_for_test();

  stop_pumping.store( true );
  pump.join();
  session.request_shutdown( ShutdownCause::CTRL_BREAK );
  runner.join();
  must( SetEvent( session_done.get() ), "SetEvent(session_done)" );
  watchdog.stop();
  console_test_clear_setup_injections();

  if ( shutdown_before_request ) {
    fprintf( stderr, "FAIL: %s: Ctrl-Z began a session shutdown\n", label );
    return 1;
  }
  if ( !echoed ) {
    fprintf( stderr, "FAIL: %s: the echoed 0x1A never reached server cell (0,0)\n", label );
    return 1;
  }
  return 0;
}

int main( int argc, char *argv[] )
{
  if ( argc < 2 ) {
    fprintf( stderr, "usage: %s <mode>\n", argv[0] );
    return 1;
  }

  /* MoshCore requires the host to establish a UTF-8 locale before construction,
     as mosh.exe and the core test both do. Without it wcrtomb() cannot encode
     the non-ASCII characters the overlay renderer produces, and the frame path
     builds a string from a failed conversion length. Only a run long enough to
     draw an overlay reaches that, which is why the modes that exit on their
     first wakeup never needed it. */
  if ( setlocale( LC_ALL, ".UTF-8" ) == NULL ) {
    fprintf( stderr, "fatal: setlocale(LC_ALL, \".UTF-8\") failed\n" );
    return 1;
  }

  try {
    if ( strcmp( argv[1], "escape-key" ) == 0 ) {
      return run_escape_key();
    }
    if ( strcmp( argv[1], "no-term-init" ) == 0 ) {
      return run_no_term_init();
    }
    if ( strcmp( argv[1], "vt-mode" ) == 0 ) {
      return run_vt_mode();
    }
    if ( strcmp( argv[1], "fairness" ) == 0 ) {
      return run_fairness();
    }
    if ( strcmp( argv[1], "clock-refresh" ) == 0 ) {
      return run_clock_refresh();
    }
    if ( strcmp( argv[1], "transaction" ) == 0 ) {
      return run_transaction();
    }
    if ( strcmp( argv[1], "init-checkpoint" ) == 0 ) {
      return run_init_checkpoint();
    }
    if ( strcmp( argv[1], "graceful-shutdown" ) == 0 ) {
      return run_graceful_shutdown();
    }
    if ( strcmp( argv[1], "reader-eof" ) == 0 ) {
      return run_reader_end( ConsoleReaderTestOutcome::END_OF_INPUT, "reader-eof" );
    }
    if ( strcmp( argv[1], "reader-failure" ) == 0 ) {
      return run_reader_end( ConsoleReaderTestOutcome::READ_FAILURE, "reader-failure" );
    }
    if ( strcmp( argv[1], "connection-timeout" ) == 0 ) {
      return run_connection_timeout();
    }
    if ( strcmp( argv[1], "upstream-length" ) == 0 ) {
      return run_upstream_length();
    }
    if ( strcmp( argv[1], "deadline-publication" ) == 0 ) {
      return run_deadline_case( DeadlineCase::PUBLICATION );
    }
    if ( strcmp( argv[1], "deadline-escalation" ) == 0 ) {
      return run_deadline_case( DeadlineCase::ESCALATION );
    }
    if ( strcmp( argv[1], "deadline-wedged-reader" ) == 0 ) {
      return run_mid_teardown_close();
    }
    if ( strcmp( argv[1], "poll-throttle" ) == 0 ) {
      return run_poll_throttle();
    }
    if ( strcmp( argv[1], "probe-ctrlz-read" ) == 0 ) {
      return run_probe_ctrlz_read();
    }
    if ( strcmp( argv[1], "ctrl-z-passthrough" ) == 0 ) {
      return run_ctrlz_passthrough( true );
    }
    if ( strcmp( argv[1], "probe-ctrlz-e2e" ) == 0 ) {
      return run_ctrlz_passthrough( false );
    }
  } catch ( const std::exception &e ) {
    fprintf( stderr, "FAIL: uncaught exception: %s\n", e.what() );
    return 1;
  } catch ( ... ) {
    fprintf( stderr, "FAIL: uncaught non-standard exception\n" );
    return 1;
  }

  fprintf( stderr, "error: unknown mode '%s'\n", argv[1] );
  return 1;
}
