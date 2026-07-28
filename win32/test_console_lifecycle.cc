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
/* ABOUTME: Modes: vt-mode asserts raw enter/restore output flags; fairness proves termination wins a contested wakeup. */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <thread>

#include "win32/console_io.h"
#include "win32/mosh_core.h"
#include "win32/test_server.h"

class ConsoleTestScope;
static ConsoleTestScope *active_console_scope = NULL;
static void must( BOOL ok, const char *what );

/* Point the process std handles at the real attached console for the duration
   of an in-process test. The msys2 CI shell redirects std handles to pipes, so
   GetStdHandle(STD_OUTPUT_HANDLE) is not the console; CONOUT$/CONIN$ always name
   the attached console. console_raw_enter reads GetStdHandle, so repointing the
   std handles lets it run its production path against the real console. stderr is
   deliberately left untouched so failure diagnostics still reach the CI log. */
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
static int run_vt_mode()
{
  ConsoleTestScope scope;   /* std handles now name the real console */

  HANDLE h_out = GetStdHandle( STD_OUTPUT_HANDLE );
  DWORD initial_out = 0;
  must( GetConsoleMode( h_out, &initial_out ), "GetConsoleMode(h_out)" );
  must( SetConsoleMode( h_out, initial_out & ~ENABLE_PROCESSED_OUTPUT ), "SetConsoleMode(clear PROCESSED_OUTPUT)" );

  ConsoleState saved;
  console_raw_enter( &saved );
  DWORD active_out_mode = 0;
  must( GetConsoleMode( saved.h_out, &active_out_mode ), "GetConsoleMode(active)" );
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
  console_raw_restore( &saved );
  DWORD restored_out_mode = 0;
  must( GetConsoleMode( saved.h_out, &restored_out_mode ), "GetConsoleMode(restored)" );
  if ( restored_out_mode != saved.out_mode ) {
    fprintf( stderr, "FAIL: mode not restored\n" );
    return 1;
  }
  return 0;
}

/* Deadline for run() to observe the already-signaled termination event. It is
   set generously on purpose: a tight deadline would measure console scheduling
   latency instead of the dispatch order and make the test flaky. The deadline
   only bounds a genuine hang; discrimination is carried by the drain-flag and
   core-finished post-conditions, and a winner-only dispatch returns well inside
   this bound. */
static const DWORD FAIRNESS_DEADLINE_MS = 10000;
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

/* Enforces the fairness deadline from a separate thread so the test thread can
   be the one inside run(). stop() and the destructor both release the thread, so
   an early return needs no special handling. */
class WatchdogGuard {
public:
  WatchdogGuard( HANDLE session_done, HANDLE deadline_start,
                 const std::atomic<ULONGLONG> *deadline_at, ConsoleTestScope *scope )
    : session_done_( session_done ),
      thread_( &WatchdogGuard::watch, session_done, deadline_start, deadline_at, scope )
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
                     const std::atomic<ULONGLONG> *deadline_at, ConsoleTestScope *scope )
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
      fprintf( stderr,
               "FAIL: session.run() did not return within %lums although "
               "termination was signaled before it was entered\n",
               FAIRNESS_DEADLINE_MS );
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

/* Fairness test: proves the event loop services termination on the same wakeup
   as a ready reader event. A winner-only dispatch favors the reader event at
   index 0 whenever it is signaled, so termination cannot win a contested
   wakeup. The fair dispatch independently re-tests termination and exits
   immediately.

   Synchronization contract: the test preloads the reader backlog to
   INPUT_BUDGET_BYTES+1 before run() begins, then atomically verifies the
   backlog and SetEvent(termination) while holding the reader queue mutex.
   This proves the old loop cannot break on its first two wakeups: the reader
   event is already signaled, and after the next drain at least one byte
   remains and re-signals it. The sticky drain flag records whether any drain
   ever emptied the queue; under the fair dispatch run() breaks before take_bytes()
   so the flag stays false, while a winner-only dispatch drains until the
   reader event goes quiet and termination wins an idle wakeup instead. */
static int run_fairness()
{
  ConsoleTestScope scope;

  ConsoleState saved;
  console_raw_enter( &saved );

  TestServer server( 80, 24 );
  MoshCore core( "127.0.0.1", server.port().c_str(), server.get_key().c_str(),
                 80, 24, "never" );

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

  auto session = std::make_unique<ConsoleSession>( core, saved );
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
  WatchdogGuard watchdog( session_done.get(), deadline_start.get(), &deadline_at, &scope );

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
      if ( !WriteConsoleInputA( saved.h_in, records, RECORDS_PER_FILL_BATCH,
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
    fprintf( stderr, "FAIL: session.run() threw: %s\n", e.what() );
    thrown = 1;
  } catch ( ... ) {
    fprintf( stderr, "FAIL: session.run() threw a non-standard exception\n" );
    thrown = 1;
  }
  must( SetEvent( session_done.get() ), "SetEvent(session_done)" );
  watchdog.stop();

  /* The sticky drain flag records whether any drain ever emptied the queue.
     A winner-only dispatch escapes by consuming the backlog until the reader
     event goes quiet and termination wins an idle wakeup instead of a
     contested one. Under the fair dispatch run() breaks at the termination
     re-test before take_bytes(), so the flag stays false. */
  const bool backlog_ever_emptied = session->input_ever_drained_for_test();
  const bool core_finished = core.is_finished();

  if ( thrown ) {
    return 1;
  }
  /* run() must have returned because it observed termination. This harness
     never services TestServer, so a run() that returns without servicing
     termination can still leave through core.is_finished() once MoshCore's
     connection timeout elapses. The watchdog deadline is stamped after the
     bounded fill and expires first, so in practice it pre-empts that path;
     this check costs nothing and names the cause if it ever wins the race. */
  if ( core_finished ) {
    fprintf( stderr,
             "FAIL: session.run() returned because the mosh core finished "
             "(connection timeout), not because it serviced termination\n" );
    return 1;
  }
  if ( backlog_ever_emptied ) {
    fprintf( stderr,
             "FAIL: reader backlog drained to empty during run(); "
             "termination may have won an idle wakeup rather than a contested "
             "one\n" );
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

  try {
    if ( strcmp( argv[1], "vt-mode" ) == 0 ) {
      return run_vt_mode();
    }
    if ( strcmp( argv[1], "fairness" ) == 0 ) {
      return run_fairness();
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
