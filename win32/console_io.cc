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

/* ABOUTME: Drives MoshCore from Windows console input, resize polling, and WinSock events. */
/* ABOUTME: Converts conhost CESU-8 input to UTF-8 before MoshCore receives it. */

#include "win32/console_io.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

namespace {
const DWORD RESIZE_POLL_CAP_MS = 100;
/* Capacity of the clock-refresh sample buffer. Recording stops at this many
   iterations; it does not wrap, so the samples are always the first ones. */
const size_t CLOCK_SAMPLE_CAPACITY = 64;
const size_t READ_BUFFER_SIZE = 4096;
const size_t INPUT_BUDGET_BYTES = 65536;
/* Bound retained paste data while leaving room for several large terminal pastes. */
const size_t INPUT_QUEUE_CAP_BYTES = 4 * 1024 * 1024;

/* Process-retained control block a control-handler callback may still be
   waiting on after the ConsoleSession that created it is gone. Never freed,
   and neither event is ever closed: a callback that already loaded the
   pointer may be about to wait on these handles, and freeing or closing
   under that wait is undefined behavior. One block and two handles leak per
   session construction: mosh.exe builds one session, though the acceptance
   harness builds several in a process. */
struct ShutdownControl {
  HANDLE termination;
  HANDLE restored;
  std::atomic<ShutdownCause> cause;
};

/* Published once setup allocates both events, cleared on every teardown path.
   The handler loads this once into a local and null-checks before use. */
std::atomic<ShutdownControl *> g_shutdown_control( nullptr );

/* Set by console_test_fail_after(); the ConsoleSetupStep to fail after, cast
   to int, or -1 for no injected failure. Consulted only after the named
   mutation has already succeeded. */
std::atomic<int> g_fail_after_step( -1 );

/* Set by console_test_signal_termination_after(); the ConsoleSetupStep after
   which to signal the real termination event, cast to int, or -1 for none.
   Unlike the failure injector this does not short-circuit: setup still reaches
   the production termination checkpoint and fails there. */
std::atomic<int> g_terminate_after_step( -1 );

/* The restored event of the most recently created control block. Readable after
   a constructor throws, when no session object survives to be asked. Safe to
   expose because control blocks and their handles are never reclaimed. */
std::atomic<HANDLE> g_last_restored_event( NULL );

std::string error_message( const char *what, DWORD error )
{
  std::ostringstream message;
  message << what << " (error " << error << ")";
  return message.str();
}

void throw_last_error( const char *what )
{
  const DWORD error = GetLastError();
  throw ConsoleError( error, error_message( what, error ) );
}

bool valid_handle( HANDLE handle )
{
  return handle != NULL && handle != INVALID_HANDLE_VALUE;
}

/* These causes have an OS-enforced termination deadline that the event loop
   cannot currently honor while completing a transport shutdown handshake. */
bool has_termination_deadline( ShutdownCause cause )
{
  return cause == ShutdownCause::CTRL_CLOSE || cause == ShutdownCause::SESSION_END;
}

/* Keeps the first failure recorded into *report; later failures are dropped
   because the caller only wants to report the failure that actually broke
   restoration, not a downstream one it caused. */
void record_last_error( CleanupReport *report, int failed_op ) noexcept
{
  if ( report->first_error == ERROR_SUCCESS ) {
    report->first_error = GetLastError();
    report->failed_op = failed_op;
  }
}

/* Backstop for callers that already have a success/failure bool: if the
   operation failed but nothing more specific was recorded, record a generic
   failure rather than leaving the report silently blank. */
void record_cleanup_failure( CleanupReport *report, bool succeeded, int failed_op ) noexcept
{
  if ( !succeeded && report->first_error == ERROR_SUCCESS ) {
    report->first_error = ERROR_WRITE_FAULT;
    report->failed_op = failed_op;
  }
}

/* write_all() for the restore path: reports failure into *report instead of
   throwing, so restoration can keep going through every remaining step. */
bool write_all_noexcept( HANDLE handle, const std::string &bytes, CleanupReport *report,
                         int failed_op ) noexcept
{
  size_t offset = 0;
  while ( offset < bytes.size() ) {
    const size_t remaining = bytes.size() - offset;
    const DWORD requested = static_cast<DWORD>( std::min<size_t>( remaining, MAXDWORD ) );
    DWORD written = 0;
    if ( !WriteFile( handle, bytes.data() + offset, requested, &written, NULL ) ) {
      record_last_error( report, failed_op );
      return false;
    }
    if ( written == 0 ) {
      /* GetLastError() is undefined for a zero-byte "success," matching the
         throwing write_all(). */
      if ( report->first_error == ERROR_SUCCESS ) {
        report->first_error = ERROR_WRITE_FAULT;
        report->failed_op = failed_op;
      }
      return false;
    }
    offset += written;
  }
  return true;
}

class Reader {
private:
  std::atomic<HANDLE> input;
  HANDLE ready_event;
  std::atomic<bool> stopping;
  mutable std::mutex queue_mutex;
  std::string queue;
  /* Sticky: set the first time a drain leaves the queue empty. Records the
     drain event itself rather than an end state, so it cannot be confused by
     whatever the queue happens to hold once the loop stops. Guarded by
     queue_mutex. */
  bool drained_to_empty;
  HANDLE worker;
  std::mutex failure_mutex;
  DWORD failure_code;
  bool failed;

  void append_utf8( std::string &output, unsigned int codepoint )
  {
    if ( codepoint <= 0x7f ) {
      output.push_back( static_cast<char>( codepoint ) );
    } else if ( codepoint <= 0x7ff ) {
      output.push_back( static_cast<char>( 0xc0 | ( codepoint >> 6 ) ) );
      output.push_back( static_cast<char>( 0x80 | ( codepoint & 0x3f ) ) );
    } else if ( codepoint <= 0xffff ) {
      output.push_back( static_cast<char>( 0xe0 | ( codepoint >> 12 ) ) );
      output.push_back( static_cast<char>( 0x80 | ( ( codepoint >> 6 ) & 0x3f ) ) );
      output.push_back( static_cast<char>( 0x80 | ( codepoint & 0x3f ) ) );
    } else {
      output.push_back( static_cast<char>( 0xf0 | ( codepoint >> 18 ) ) );
      output.push_back( static_cast<char>( 0x80 | ( ( codepoint >> 12 ) & 0x3f ) ) );
      output.push_back( static_cast<char>( 0x80 | ( ( codepoint >> 6 ) & 0x3f ) ) );
      output.push_back( static_cast<char>( 0x80 | ( codepoint & 0x3f ) ) );
    }
  }

  std::string recombine_cesu8( const char *bytes, size_t length,
                               std::string &pending )
  {
    std::string input = pending;
    input.append( bytes, length );
    pending.clear();
    std::string output;

    for ( size_t pos = 0; pos < input.size(); ) {
      const unsigned char first = static_cast<unsigned char>( input[pos] );
      if ( first != 0xed ) {
        output.push_back( input[pos++] );
        continue;
      }
      if ( pos + 1 == input.size() ) {
        pending.assign( input, pos, 1 );
        break;
      }
      const unsigned char second = static_cast<unsigned char>( input[pos + 1] );
      if ( ( second & 0xf0 ) != 0xa0 ) {
        output.push_back( input[pos++] );
        continue;
      }
      if ( pos + 2 == input.size() ) {
        pending.assign( input, pos, 2 );
        break;
      }
      const unsigned char third = static_cast<unsigned char>( input[pos + 2] );
      if ( ( third & 0xc0 ) != 0x80 ) {
        output.push_back( input[pos++] );
        continue;
      }

      const unsigned int high = ( ( first & 0x0f ) << 12 )
        | ( ( second & 0x3f ) << 6 ) | ( third & 0x3f );
      if ( pos + 3 == input.size() ) {
        pending.assign( input, pos, 3 );
        break;
      }
      if ( static_cast<unsigned char>( input[pos + 3] ) != 0xed ) {
        output.append( input, pos, 3 );
        pos += 3;
        continue;
      }
      if ( pos + 4 == input.size() ) {
        pending.assign( input, pos, 4 );
        break;
      }
      const unsigned char low_second = static_cast<unsigned char>( input[pos + 4] );
      if ( ( low_second & 0xf0 ) != 0xb0 ) {
        output.append( input, pos, 3 );
        pos += 3;
        continue;
      }
      if ( pos + 5 == input.size() ) {
        pending.assign( input, pos, 5 );
        break;
      }
      const unsigned char low_third = static_cast<unsigned char>( input[pos + 5] );
      if ( ( low_third & 0xc0 ) == 0x80 ) {
        const unsigned int low = ( 0x0d << 12 ) | ( ( low_second & 0x3f ) << 6 )
          | ( low_third & 0x3f );
        append_utf8( output, 0x10000 + ( ( high - 0xd800 ) << 10 ) + low - 0xdc00 );
        pos += 6;
      } else {
        output.append( input, pos, 3 );
        pos += 3;
      }
    }
    return output;
  }

  static DWORD WINAPI start( LPVOID parameter )
  {
    static_cast<Reader *>( parameter )->read_loop();
    return 0;
  }

  void read_loop()
  {
    char bytes[READ_BUFFER_SIZE];
    std::string pending;
    while ( !stopping.load() ) {
      DWORD read = 0;
      if ( !ReadFile( input.load(), bytes, sizeof bytes, &read, NULL ) ) {
        const DWORD error = GetLastError();
        if ( stopping.load() ) {
          break;
        }
        std::lock_guard<std::mutex> lock( failure_mutex );
        failure_code = error;
        failed = true;
        SetEvent( ready_event );
        break;
      }
      if ( read == 0 ) {
        continue;
      }
      std::string converted = recombine_cesu8( bytes, read, pending );
      while ( !converted.empty() && !stopping.load() ) {
        bool enqueued = false;
        {
          std::lock_guard<std::mutex> lock( queue_mutex );
          /* Preserve paste bytes: pause console reads at the cap until the main
             loop drains space, rather than growing memory or dropping input. */
          if ( queue.size() + converted.size() <= INPUT_QUEUE_CAP_BYTES ) {
            queue += converted;
            SetEvent( ready_event );
            enqueued = true;
          }
        }
        if ( enqueued ) {
          break;
        }
        Sleep( 1 );
      }
    }
  }

public:
  explicit Reader( HANDLE h_in )
    : input( NULL ), ready_event( CreateEvent( NULL, FALSE, FALSE, NULL ) ),
      stopping( false ), drained_to_empty( false ), worker( NULL ),
      failure_code( ERROR_SUCCESS ), failed( false )
  {
    if ( ready_event == NULL ) {
      throw_last_error( "CreateEvent for console input" );
    }
    HANDLE duplicate = NULL;
    if ( !DuplicateHandle( GetCurrentProcess(), h_in, GetCurrentProcess(), &duplicate,
                           0, FALSE, DUPLICATE_SAME_ACCESS ) ) {
      const DWORD error = GetLastError();
      CloseHandle( ready_event );
      throw ConsoleError( error, error_message( "DuplicateHandle console input", error ) );
    }
    input.store( duplicate );
    worker = CreateThread( NULL, 0, &Reader::start, this, 0, NULL );
    if ( worker == NULL ) {
      const DWORD error = GetLastError();
      CloseHandle( input.exchange( NULL ) );
      CloseHandle( ready_event );
      throw ConsoleError( error, error_message( "CreateThread for console input", error ) );
    }
  }

  ~Reader()
  {
    stop();
    CloseHandle( ready_event );
  }

  HANDLE event() const
  {
    return ready_event;
  }

  std::string take_bytes( size_t limit )
  {
    std::lock_guard<std::mutex> lock( queue_mutex );
    const size_t count = std::min( limit, queue.size() );
    std::string bytes = queue.substr( 0, count );
    queue.erase( 0, count );
    if ( !queue.empty() ) {
      SetEvent( ready_event );
    } else {
      drained_to_empty = true;
    }
    return bytes;
  }

  void check_failure()
  {
    std::lock_guard<std::mutex> lock( failure_mutex );
    if ( failed ) {
      throw ConsoleError( failure_code, error_message( "ReadFile console input", failure_code ) );
    }
  }

  size_t pending_bytes() const
  {
    std::lock_guard<std::mutex> lock( queue_mutex );
    return queue.size();
  }

  bool ever_drained_to_empty() const
  {
    std::lock_guard<std::mutex> lock( queue_mutex );
    return drained_to_empty;
  }

  /* Locks the real reader queue, verifies the backlog meets the minimum, and
     signals the termination event atomically — used by the fairness test to
     prove the reader event is already signaled and at least one budget remains
     after the next drain. Returns true if the minimum was met. */
  bool signal_termination_against_backlog( HANDLE event, size_t minimum )
  {
    std::lock_guard<std::mutex> lock( queue_mutex );
    const bool ok = queue.size() >= minimum;
    if ( ok ) {
      SetEvent( event );
    }
    return ok;
  }

  void stop()
  {
    if ( worker != NULL ) {
      stopping.store( true );
      while ( WaitForSingleObject( worker, 100 ) == WAIT_TIMEOUT ) {
        CancelSynchronousIo( worker );
        HANDLE handle = input.exchange( NULL );
        if ( handle != NULL ) {
          CloseHandle( handle );
        }
      }
      HANDLE handle = input.exchange( NULL );
      if ( handle != NULL ) {
        CloseHandle( handle );
      }
      CloseHandle( worker );
      worker = NULL;
    }
  }
};

class SocketEvents {
private:
  MoshCore &core;
  std::map<intptr_t, WSAEVENT> events;

public:
  explicit SocketEvents( MoshCore &session_core ) : core( session_core ) {}

  ~SocketEvents()
  {
    const std::vector<intptr_t> live_fds = core.socket_fds();
    for ( std::map<intptr_t, WSAEVENT>::iterator it = events.begin(); it != events.end(); ++it ) {
      if ( std::find( live_fds.begin(), live_fds.end(), it->first ) != live_fds.end() ) {
        WSAEventSelect( static_cast<SOCKET>( it->first ), NULL, 0 );
      }
      WSACloseEvent( it->second );
    }
  }

  void reconcile( const std::vector<intptr_t> &fds )
  {
    for ( std::map<intptr_t, WSAEVENT>::iterator it = events.begin(); it != events.end(); ) {
      if ( std::find( fds.begin(), fds.end(), it->first ) == fds.end() ) {
        WSACloseEvent( it->second );
        it = events.erase( it );
      } else {
        ++it;
      }
    }
    for ( std::vector<intptr_t>::const_iterator it = fds.begin(); it != fds.end(); ++it ) {
      if ( events.find( *it ) != events.end() ) {
        continue;
      }
      WSAEVENT event = WSACreateEvent();
      if ( event == WSA_INVALID_EVENT ) {
        const int error = WSAGetLastError();
        throw ConsoleError( static_cast<DWORD>( error ), error_message( "WSACreateEvent", error ) );
      }
      if ( WSAEventSelect( static_cast<SOCKET>( *it ), event, FD_READ ) == SOCKET_ERROR ) {
        const int error = WSAGetLastError();
        WSACloseEvent( event );
        throw ConsoleError( static_cast<DWORD>( error ), error_message( "WSAEventSelect", error ) );
      }
      events[*it] = event;
    }
  }

  const std::map<intptr_t, WSAEVENT> &all() const
  {
    return events;
  }
};
}

BOOL WINAPI console_control_handler( DWORD type )
{
  ShutdownCause cause;
  switch ( type ) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
    cause = ShutdownCause::CTRL_BREAK;
    break;
  case CTRL_CLOSE_EVENT:
  case CTRL_LOGOFF_EVENT:
  case CTRL_SHUTDOWN_EVENT:
    cause = ShutdownCause::CTRL_CLOSE;
    break;
  default:
    return FALSE;
  }
  /* Load once into a local: the global can go NULL the instant after this
     handler loads it, but never while this local still holds the pointer, so
     a single null-checked local is all the safety this needs. */
  ShutdownControl *control = g_shutdown_control.load();
  if ( control != NULL ) {
    control->cause.store( cause );
    SetEvent( control->termination );
  }
  return TRUE;
}

void console_test_fail_after( ConsoleSetupStep step )
{
  g_fail_after_step.store( static_cast<int>( step ) );
}

void console_test_signal_termination_after( ConsoleSetupStep step )
{
  g_terminate_after_step.store( static_cast<int>( step ) );
}

void console_test_clear_setup_injections()
{
  g_fail_after_step.store( -1 );
  g_terminate_after_step.store( -1 );
}

HANDLE console_test_last_restored_event()
{
  return g_last_restored_event.load();
}

void console_dims( int *cols, int *rows )
{
  HANDLE output = GetStdHandle( STD_OUTPUT_HANDLE );
  CONSOLE_SCREEN_BUFFER_INFO info;
  if ( !valid_handle( output ) || !GetConsoleScreenBufferInfo( output, &info ) ) {
    throw_last_error( "GetConsoleScreenBufferInfo" );
  }
  *cols = info.srWindow.Right - info.srWindow.Left + 1;
  *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
}

void write_all( HANDLE handle, const std::string &bytes )
{
  size_t offset = 0;
  while ( offset < bytes.size() ) {
    const size_t remaining = bytes.size() - offset;
    const DWORD requested = static_cast<DWORD>( std::min<size_t>( remaining, MAXDWORD ) );
    DWORD written = 0;
    if ( !WriteFile( handle, bytes.data() + offset, requested, &written, NULL ) ) {
      throw_last_error( "WriteFile console output" );
    }
    if ( written == 0 ) {
      throw ConsoleError( ERROR_WRITE_FAULT, "WriteFile console output made no progress" );
    }
    offset += written;
  }
}

class ConsoleSession::Impl {
public:
  MoshCore &core;
  ConsoleSnapshot original;
  bool input_mode_set;
  bool output_mode_set;
  bool input_cp_set;
  bool output_cp_set;
  bool open_sequence_started;
  /* Non-owning: deliberately leaked, see ShutdownControl above. Valid from
     the point setup allocates it (early in the constructor) onward; never
     reassigned after that. */
  ShutdownControl *control;
  std::unique_ptr<Reader> reader;
  std::unique_ptr<SocketEvents> socket_events;
  int cols;
  int rows;
  ConsoleLifecycleState state;
  /* Guards restore() against running twice: once from run()'s exit path, once
     more from the destructor if run() was never called. */
  bool restored_flag;
  CleanupReport cleanup;

  /* Per-iteration clock samples for the clock-refresh test. Written from inside
     run() without a lock, so the accessor's contract forbids reading them while
     run() is executing. */
  ConsoleSession::ClockRefreshSample clock_samples[CLOCK_SAMPLE_CAPACITY];
  size_t clock_sample_count;

  /* State for graceful shutdown: true once the loop has observed a shutdown
     request and begun the graceful shutdown. Set exactly once on the first
     wakeup where the termination event is signaled. */
  bool shutdown_observed;
  /* Whether a drain had ever emptied the input queue at the moment the loop
     first observed a shutdown request. Sampled at that instant rather than
     after run() returns, because the loop keeps pumping afterward and will
     drain the queue in the normal course of shutting down. */
  bool input_drained_at_shutdown;

  explicit Impl( MoshCore &core_ref )
    : core( core_ref ), original(), input_mode_set( false ), output_mode_set( false ),
      input_cp_set( false ), output_cp_set( false ), open_sequence_started( false ),
      control( NULL ), reader(), socket_events(), cols( 0 ), rows( 0 ),
      state( ConsoleLifecycleState::RUNNING ), restored_flag( false ), cleanup(),
      clock_samples(), clock_sample_count( 0 ),
      shutdown_observed( false ), input_drained_at_shutdown( false )
  {
    original.input = GetStdHandle( STD_INPUT_HANDLE );
    original.output = GetStdHandle( STD_OUTPUT_HANDLE );
    if ( !valid_handle( original.input ) || !valid_handle( original.output ) ) {
      throw ConsoleError( ERROR_INVALID_HANDLE, "standard input or output is not a console handle" );
    }
    if ( !GetConsoleMode( original.input, &original.input_mode ) ) {
      throw_last_error( "GetConsoleMode stdin" );
    }
    if ( !GetConsoleMode( original.output, &original.output_mode ) ) {
      throw_last_error( "GetConsoleMode stdout" );
    }
    original.input_cp = GetConsoleCP();
    if ( original.input_cp == 0 ) {
      throw_last_error( "GetConsoleCP" );
    }
    original.output_cp = GetConsoleOutputCP();
    if ( original.output_cp == 0 ) {
      throw_last_error( "GetConsoleOutputCP" );
    }

    CONSOLE_SCREEN_BUFFER_INFO initial_info;
    if ( !GetConsoleScreenBufferInfo( original.output, &initial_info ) ) {
      throw_last_error( "GetConsoleScreenBufferInfo" );
    }
    cols = initial_info.srWindow.Right - initial_info.srWindow.Left + 1;
    rows = initial_info.srWindow.Bottom - initial_info.srWindow.Top + 1;
    core.resize( cols, rows );

    /* Nothing above this point mutated the console or registered anything
       process-global, so every failure so far is a plain throw: there is
       nothing yet to roll back and no restored event yet to signal. */

    control = new ShutdownControl;
    control->termination = CreateEvent( NULL, TRUE, FALSE, NULL );
    if ( control->termination == NULL ) {
      throw_last_error( "CreateEvent for console termination" );
    }
    control->restored = CreateEvent( NULL, TRUE, FALSE, NULL );
    if ( control->restored == NULL ) {
      throw_last_error( "CreateEvent for console restored" );
    }
    control->cause.store( ShutdownCause::IN_BAND );
    g_last_restored_event.store( control->restored );

    /* Publish before registering. A control event delivered in the gap between
       the handler becoming callable and the pointer becoming visible would load
       NULL, report the event handled, and drop it. Nothing is mutated yet, so a
       registration failure only has to unpublish. */
    g_shutdown_control.store( control );
    if ( !SetConsoleCtrlHandler( console_control_handler, TRUE ) ) {
      const DWORD error = GetLastError();
      g_shutdown_control.store( NULL );
      throw ConsoleError( error, error_message( "SetConsoleCtrlHandler", error ) );
    }

    /* Everything below mutates the console or owns a thread. A constructor that
       throws does not run ~Impl, so this catch is the only unwind path: it has to
       cover an allocation failure or any other non-ConsoleError as well, or a
       failure here would leave the console raw and the handler registered. */
    try {
      setup_console();
    } catch ( ... ) {
      release_and_signal();
      throw;
    }
  }

  /* Applies the console mutations, starts the reader, and enters the alternate
     screen. Each mutation is recorded before its checkpoint so a rollback undoes
     exactly what was applied. */
  void setup_console()
  {
    const DWORD raw_input = ( original.input_mode | ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS )
      & ~( ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE );
    if ( !SetConsoleMode( original.input, raw_input ) ) {
      const DWORD error = GetLastError();
      fail_setup( error, error_message( "SetConsoleMode stdin", error ) );
    }
    input_mode_set = true;
    checkpoint( ConsoleSetupStep::INPUT_MODE );

    const DWORD vt_output = original.output_mode | ENABLE_PROCESSED_OUTPUT
      | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
    if ( !SetConsoleMode( original.output, vt_output ) ) {
      const DWORD error = GetLastError();
      fail_setup( error, error_message( "SetConsoleMode stdout", error ) );
    }
    output_mode_set = true;
    checkpoint( ConsoleSetupStep::OUTPUT_MODE );

    if ( !SetConsoleCP( CP_UTF8 ) ) {
      const DWORD error = GetLastError();
      fail_setup( error, error_message( "SetConsoleCP", error ) );
    }
    input_cp_set = true;
    checkpoint( ConsoleSetupStep::INPUT_CODE_PAGE );

    if ( !SetConsoleOutputCP( CP_UTF8 ) ) {
      const DWORD error = GetLastError();
      fail_setup( error, error_message( "SetConsoleOutputCP", error ) );
    }
    output_cp_set = true;
    checkpoint( ConsoleSetupStep::OUTPUT_CODE_PAGE );

    try {
      reader.reset( new Reader( original.input ) );
    } catch ( const ConsoleError &error ) {
      fail_setup( error.win32_code, error.what() );
    }
    socket_events.reset( new SocketEvents( core ) );

    /* Set before the write: a partial write may already have entered the
       alternate screen, so restore() must issue the close sequence even if
       this write fails partway through. */
    open_sequence_started = true;
    try {
      write_all( original.output, core.open_sequence() );
    } catch ( const ConsoleError &error ) {
      fail_setup( error.win32_code, error.what() );
    }
  }

  ~Impl()
  {
    release_and_signal();
  }

  /* Stops the reader, rolls back whatever setup applied, signals restored, and
     releases the process-global registration. Idempotent, so the constructor's
     unwind path, run(), and the destructor can each call it and only the first
     has any effect. noexcept because a control handler is blocked on restored
     and an exception escaping here would strand it.

     The reader stops first: restore() returns the console to cooked input, and a
     worker still blocked in ReadFile on CONIN$ could otherwise swallow typeahead
     after the console has been declared restored. */
  void release_and_signal() noexcept
  {
    if ( restored_flag ) {
      return;
    }
    restored_flag = true;
    if ( reader ) {
      reader->stop();
    }
    cleanup = restore();
    state = ConsoleLifecycleState::RESTORED;
    SetEvent( control->restored );
    SetConsoleCtrlHandler( console_control_handler, FALSE );
    g_shutdown_control.store( NULL );
  }

  /* Tests whether test-injected or real termination has fired after the
     named mutation succeeded, and if so rolls back and throws. Only the four
     console mutations are checkpointed; the open-sequence write is not. */
  void checkpoint( ConsoleSetupStep step )
  {
    /* Signalling rather than short-circuiting: a test that wants the termination
       branch has to reach it through the same zero-timeout wait production uses,
       otherwise the branch could be deleted and the test would still pass. */
    if ( g_terminate_after_step.load() == static_cast<int>( step ) ) {
      SetEvent( control->termination );
    }
    if ( g_fail_after_step.load() == static_cast<int>( step ) ) {
      fail_setup( ERROR_CANCELLED, "console setup test-injected failure" );
    }
    if ( WaitForSingleObject( control->termination, 0 ) == WAIT_OBJECT_0 ) {
      fail_setup( ERROR_OPERATION_ABORTED, "console setup aborted by termination request" );
    }
  }

  /* Rolls back through release_and_signal(), then reports why setup stopped. The
     constructor's catch also calls release_and_signal(), which is idempotent, so
     the rollback happens exactly once whichever route the failure takes. */
  void fail_setup( DWORD error, const std::string &message )
  {
    release_and_signal();
    std::string full_message = message;
    if ( cleanup.first_error != ERROR_SUCCESS ) {
      std::ostringstream detail;
      detail << full_message << "; console rollback failed (error " << cleanup.first_error << ")";
      full_message = detail.str();
    }
    throw ConsoleError( error, full_message );
  }

  /* Best-effort and allocation-free: restores whatever has actually been
     applied, keeping only the first failure. Safe to call before any
     mutation has been applied (every flag is false, so every step is a
     no-op) and safe to call more than once. */
  CleanupReport restore() noexcept
  {
    CleanupReport report = { ERROR_SUCCESS, CLEANUP_OP_NONE };
    if ( open_sequence_started ) {
      record_cleanup_failure( &report,
        write_all_noexcept( original.output, core.close_sequence(), &report, CLEANUP_OP_CLOSE_SEQUENCE ),
        CLEANUP_OP_CLOSE_SEQUENCE );
    }
    if ( output_cp_set && !SetConsoleOutputCP( original.output_cp ) ) {
      record_last_error( &report, CLEANUP_OP_OUTPUT_CP );
    }
    if ( input_cp_set && !SetConsoleCP( original.input_cp ) ) {
      record_last_error( &report, CLEANUP_OP_INPUT_CP );
    }
    if ( output_mode_set && !SetConsoleMode( original.output, original.output_mode ) ) {
      record_last_error( &report, CLEANUP_OP_OUTPUT_MODE );
    }
    if ( input_mode_set && !SetConsoleMode( original.input, original.input_mode ) ) {
      record_last_error( &report, CLEANUP_OP_INPUT_MODE );
    }
    return report;
  }

  HANDLE termination_event() const
  {
    return control->termination;
  }

  HANDLE restored_event() const
  {
    return control->restored;
  }

  ConsoleSnapshot original_console() const
  {
    return original;
  }

  size_t input_backlog() const
  {
    return reader->pending_bytes();
  }

  bool input_ever_drained() const
  {
    return reader->ever_drained_to_empty();
  }

  bool signal_termination_against_backlog( size_t minimum )
  {
    return reader->signal_termination_against_backlog( control->termination, minimum );
  }

  bool shutdown_observed_for_test() const
  {
    return shutdown_observed;
  }

  bool input_drained_at_shutdown_for_test() const
  {
    return input_drained_at_shutdown;
  }

  size_t clock_refresh_samples( ConsoleSession::ClockRefreshSample *out,
                                size_t capacity ) const
  {
    const size_t count = std::min( capacity, clock_sample_count );
    for ( size_t i = 0; i < count; ++i ) {
      out[i] = clock_samples[i];
    }
    return count;
  }

  CleanupReport cleanup_report() const
  {
    return cleanup;
  }

  /* The sole cross-thread entry point. Everything else touching core, the
     reader, or console state stays on the thread inside run(). */
  void request_shutdown( ShutdownCause cause )
  {
    control->cause.store( cause );
    SetEvent( control->termination );
  }

  void run()
  {
    try {
      run_loop();
    } catch ( ... ) {
      release_and_signal();
      throw;
    }
    release_and_signal();
  }

private:
  /* The exact sequence run() and the destructor both need on every exit path:
     restore, mark RESTORED, then unconditionally signal restored. A control
     handler blocks on this event, so nothing that can throw or allocate may
     sit between restore() returning and the SetEvent below. */
  void run_loop()
  {
    while ( true ) {
      const int timeout = core.tick();
      if ( core.is_finished() ) {
        break;
      }

      socket_events->reconcile( core.socket_fds() );
      const std::map<intptr_t, WSAEVENT> &registered = socket_events->all();

      std::vector<HANDLE> handles;
      std::vector<intptr_t> fds;
      handles.push_back( reader->event() );
      /* After the termination request has been observed, the termination handle
         is no longer pushed into the wait set. This avoids a busy spin since the
         manual-reset event stays signaled. Before that observation, the resize
         poll cap bounds how long the loop can go without noticing a control event. */
      if ( !shutdown_observed ) {
        handles.push_back( control->termination );
      }
      /* Capture the base immediately after fixed handles are pushed, before sockets.
         The socket base is the count of fixed handles, which is either 2 (reader
         + termination) or 1 (reader only, after shutdown observed). This is the
         number of handles that come before the socket handles in the array. */
      const size_t socket_base = handles.size();
      for ( std::map<intptr_t, WSAEVENT>::const_iterator it = registered.begin(); it != registered.end(); ++it ) {
        handles.push_back( it->second );
        fds.push_back( it->first );
      }
      /* WaitForMultipleObjects accepts at most MAXIMUM_WAIT_OBJECTS handles. */
      if ( handles.size() > MAXIMUM_WAIT_OBJECTS ) {
        throw ConsoleError( ERROR_TOO_MANY_OPEN_FILES, "too many handles for WaitForMultipleObjects" );
      }
      const DWORD wait_timeout = static_cast<DWORD>( std::max( 0, std::min( timeout, static_cast<int>( RESIZE_POLL_CAP_MS ) ) ) );
      const DWORD result = WaitForMultipleObjects( static_cast<DWORD>( handles.size() ), &handles[0], FALSE, wait_timeout );
      if ( result == WAIT_FAILED ) {
        throw_last_error( "WaitForMultipleObjects" );
      }
      if ( result != WAIT_TIMEOUT && ( result < WAIT_OBJECT_0 || result >= WAIT_OBJECT_0 + handles.size() ) ) {
        throw ConsoleError( ERROR_INVALID_HANDLE, "WaitForMultipleObjects returned an invalid result" );
      }

      /* Refresh the cached timestamp after the wait returns and before dispatching
         any source, so transport timing sees when the event arrived rather than
         when the wait began. The wait blocks for up to RESIZE_POLL_CAP_MS, so
         dispatching on the pre-wait timestamp would backdate inbound packets and
         understate RTT. Upstream does the same at src/util/select.h:186, where
         Select::select() freezes the timestamp after pselect() returns. */
      const uint64_t tick_ts = core.cached_timestamp();
      core.refresh_clock();

      if ( clock_sample_count < CLOCK_SAMPLE_CAPACITY ) {
        ConsoleSession::ClockRefreshSample &sample = clock_samples[clock_sample_count];
        sample.tick_ts = tick_ts;
        sample.dispatch_ts = core.cached_timestamp();
        ++clock_sample_count;
      }

      /* WaitForMultipleObjects reports one winner rather than every ready
         source, so its result is only a wakeup: a continuously ready reader
         event would otherwise starve termination forever. Re-test each source
         independently instead of trusting the winner, and drain the reader queue
         directly because its auto-reset event may already have been consumed.
         Upstream gets this from pselect, which reports the whole ready set.
         Sockets are the one exception: at most one readable socket is serviced
         per wakeup, because on_readable can prune the set this snapshot names. */
      /* The termination event is only checked before shutdown has been observed.
         Once observed, the loop continues to pump until core.is_finished() and
         the termination handle has been dropped from the wait set. */
      if ( !shutdown_observed && WaitForSingleObject( control->termination, 0 ) == WAIT_OBJECT_0 ) {
        const ShutdownCause cause = control->cause.load();
        if ( has_termination_deadline( cause ) ) {
          break;
        }
        shutdown_observed = true;
        input_drained_at_shutdown = reader->ever_drained_to_empty();
        core.begin_shutdown();
        state = ConsoleLifecycleState::SHUTTING_DOWN;
      }
      /* A reader failure is fatal until shutdown has been accepted. Once the
         loop is pumping the shutdown handshake, console teardown can invalidate
         the reader before its next ReadFile completes. */
      if ( !shutdown_observed ) {
        reader->check_failure();
      }

      /* Windows polls dimensions because it has no SIGWINCH. A dying console
         can reject the query after shutdown begins, but that must not interrupt
         the transport handshake. */
      CONSOLE_SCREEN_BUFFER_INFO info;
      if ( GetConsoleScreenBufferInfo( original.output, &info ) ) {
        const int new_cols = info.srWindow.Right - info.srWindow.Left + 1;
        const int new_rows = info.srWindow.Bottom - info.srWindow.Top + 1;
        if ( new_cols != cols || new_rows != rows ) {
          cols = new_cols;
          rows = new_rows;
          core.resize( cols, rows );
        }
      } else if ( !shutdown_observed ) {
        throw_last_error( "GetConsoleScreenBufferInfo" );
      }

      for ( size_t index = 0; index < fds.size(); ++index ) {
        if ( WaitForSingleObject( handles[socket_base + index], 0 ) == WAIT_OBJECT_0 ) {
          WSANETWORKEVENTS events = {};
          if ( WSAEnumNetworkEvents( static_cast<SOCKET>( fds[index] ),
                                     handles[socket_base + index], &events ) == SOCKET_ERROR ) {
            const int error = WSAGetLastError();
            throw ConsoleError( static_cast<DWORD>( error ),
                                error_message( "WSAEnumNetworkEvents", error ) );
          }
          if ( events.lNetworkEvents & FD_READ ) {
            core.on_readable( fds[index] );
            /* on_readable may prune sockets, invalidating this wakeup's snapshot. */
            break;
          }
        }
      }
      const std::string bytes = reader->take_bytes( INPUT_BUDGET_BYTES );
      if ( !bytes.empty() ) {
        core.feed_input( bytes.data(), bytes.size() );
      }

      const std::string &frame = core.next_frame();
      if ( !frame.empty() ) {
        write_all( original.output, frame );
      }
    }
  }
};

ConsoleSession::ConsoleSession( MoshCore &core )
  : impl( new Impl( core ) )
{
}

ConsoleSession::~ConsoleSession() = default;

void ConsoleSession::run()
{
  impl->run();
}

void ConsoleSession::request_shutdown( ShutdownCause cause )
{
  impl->request_shutdown( cause );
}

HANDLE ConsoleSession::termination_event_for_test() const
{
  return impl->termination_event();
}

HANDLE ConsoleSession::restored_event_for_test() const
{
  return impl->restored_event();
}

ConsoleSnapshot ConsoleSession::original_console_for_test() const
{
  return impl->original_console();
}

size_t ConsoleSession::input_backlog_for_test() const
{
  return impl->input_backlog();
}

bool ConsoleSession::input_ever_drained_for_test() const
{
  return impl->input_ever_drained();
}

bool ConsoleSession::shutdown_observed_for_test() const
{
  return impl->shutdown_observed_for_test();
}

bool ConsoleSession::input_drained_at_shutdown_for_test() const
{
  return impl->input_drained_at_shutdown_for_test();
}

size_t ConsoleSession::input_budget_bytes()
{
  return INPUT_BUDGET_BYTES;
}

bool ConsoleSession::signal_termination_against_backlog_for_test( size_t minimum )
{
  return impl->signal_termination_against_backlog( minimum );
}

size_t ConsoleSession::clock_refresh_samples_for_test( ClockRefreshSample *out, size_t capacity ) const
{
  return impl->clock_refresh_samples( out, capacity );
}

CleanupReport ConsoleSession::cleanup_report() const
{
  return impl->cleanup_report();
}
