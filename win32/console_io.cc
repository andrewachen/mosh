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
#include <climits>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

namespace {
const DWORD RESIZE_POLL_CAP_MS = 100;
const size_t READ_BUFFER_SIZE = 4096;
const size_t INPUT_BUDGET_BYTES = 65536;
/* Bound retained paste data while leaving room for several large terminal pastes. */
const size_t INPUT_QUEUE_CAP_BYTES = 4 * 1024 * 1024;
static HANDLE g_termination_event = NULL;

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

BOOL WINAPI ctrl_handler( DWORD type )
{
  switch ( type ) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
  case CTRL_CLOSE_EVENT:
  case CTRL_LOGOFF_EVENT:
  case CTRL_SHUTDOWN_EVENT:
    if ( g_termination_event != NULL ) {
      SetEvent( g_termination_event );
    }
    return TRUE;
  default:
    return FALSE;
  }
}

class ControlHandlerGuard {
private:
  HANDLE event;

public:
  ControlHandlerGuard()
    : event( CreateEvent( NULL, TRUE, FALSE, NULL ) )
  {
    if ( event == NULL ) {
      throw_last_error( "CreateEvent for console termination" );
    }
    g_termination_event = event;
    if ( !SetConsoleCtrlHandler( ctrl_handler, TRUE ) ) {
      const DWORD error = GetLastError();
      g_termination_event = NULL;
      CloseHandle( event );
      throw ConsoleError( error, error_message( "SetConsoleCtrlHandler", error ) );
    }
  }

  ~ControlHandlerGuard()
  {
    SetConsoleCtrlHandler( ctrl_handler, FALSE );
    g_termination_event = NULL;
    CloseHandle( event );
  }

  HANDLE handle() const
  {
    return event;
  }
};

DWORD restore_console( const ConsoleState &saved, bool restore_input_mode,
                       bool restore_output_mode, bool restore_input_cp,
                       bool restore_output_cp )
{
  DWORD failure = ERROR_SUCCESS;
  if ( restore_output_cp && !SetConsoleOutputCP( saved.out_cp ) ) {
    failure = GetLastError();
  }
  if ( restore_input_cp && !SetConsoleCP( saved.in_cp ) && failure == ERROR_SUCCESS ) {
    failure = GetLastError();
  }
  if ( restore_output_mode && !SetConsoleMode( saved.h_out, saved.out_mode )
       && failure == ERROR_SUCCESS ) {
    failure = GetLastError();
  }
  if ( restore_input_mode && !SetConsoleMode( saved.h_in, saved.in_mode )
       && failure == ERROR_SUCCESS ) {
    failure = GetLastError();
  }
  return failure;
}

std::string rollback_message( const char *what, DWORD error, DWORD rollback_error )
{
  std::string message = error_message( what, error );
  if ( rollback_error != ERROR_SUCCESS ) {
    std::ostringstream rollback;
    rollback << message << "; console rollback failed (error " << rollback_error << ")";
    return rollback.str();
  }
  return message;
}

class Reader {
private:
  std::atomic<HANDLE> input;
  HANDLE ready_event;
  std::atomic<bool> stopping;
  std::mutex queue_mutex;
  std::string queue;
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
      stopping( false ), worker( NULL ), failure_code( ERROR_SUCCESS ), failed( false )
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

void console_raw_enter( ConsoleState *saved )
{
  ConsoleState captured;
  captured.h_in = GetStdHandle( STD_INPUT_HANDLE );
  captured.h_out = GetStdHandle( STD_OUTPUT_HANDLE );
  if ( !valid_handle( captured.h_in ) || !valid_handle( captured.h_out ) ) {
    throw ConsoleError( ERROR_INVALID_HANDLE, "standard input or output is not a console handle" );
  }
  if ( !GetConsoleMode( captured.h_in, &captured.in_mode ) ) {
    throw_last_error( "GetConsoleMode stdin" );
  }
  if ( !GetConsoleMode( captured.h_out, &captured.out_mode ) ) {
    throw_last_error( "GetConsoleMode stdout" );
  }
  captured.in_cp = GetConsoleCP();
  if ( captured.in_cp == 0 ) {
    throw_last_error( "GetConsoleCP" );
  }
  captured.out_cp = GetConsoleOutputCP();
  if ( captured.out_cp == 0 ) {
    throw_last_error( "GetConsoleOutputCP" );
  }

  bool input_mode = false;
  bool output_mode = false;
  bool input_cp = false;
  bool output_cp = false;
  const DWORD raw_input = ( captured.in_mode | ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS )
    & ~( ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE );
  const DWORD vt_output = captured.out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING
    | DISABLE_NEWLINE_AUTO_RETURN;
  if ( !SetConsoleMode( captured.h_in, raw_input ) ) {
    throw_last_error( "SetConsoleMode stdin" );
  }
  input_mode = true;
  if ( !SetConsoleMode( captured.h_out, vt_output ) ) {
    const DWORD error = GetLastError();
    const DWORD rollback_error = restore_console( captured, input_mode, output_mode, input_cp, output_cp );
    throw ConsoleError( error, rollback_message( "SetConsoleMode stdout", error, rollback_error ) );
  }
  output_mode = true;
  if ( !SetConsoleCP( CP_UTF8 ) ) {
    const DWORD error = GetLastError();
    const DWORD rollback_error = restore_console( captured, input_mode, output_mode, input_cp, output_cp );
    throw ConsoleError( error, rollback_message( "SetConsoleCP", error, rollback_error ) );
  }
  input_cp = true;
  if ( !SetConsoleOutputCP( CP_UTF8 ) ) {
    const DWORD error = GetLastError();
    const DWORD rollback_error = restore_console( captured, input_mode, output_mode, input_cp, output_cp );
    throw ConsoleError( error, rollback_message( "SetConsoleOutputCP", error, rollback_error ) );
  }
  output_cp = true;
  *saved = captured;
}

void console_raw_restore( const ConsoleState *saved )
{
  SetConsoleOutputCP( saved->out_cp );
  SetConsoleCP( saved->in_cp );
  SetConsoleMode( saved->h_out, saved->out_mode );
  SetConsoleMode( saved->h_in, saved->in_mode );
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

void console_run( MoshCore &core, const ConsoleState &cs )
{
  ControlHandlerGuard termination_guard;
  SocketEvents socket_events( core );
  Reader reader( cs.h_in );
  CONSOLE_SCREEN_BUFFER_INFO initial_info;
  if ( !GetConsoleScreenBufferInfo( cs.h_out, &initial_info ) ) {
    throw_last_error( "GetConsoleScreenBufferInfo" );
  }
  int cols = initial_info.srWindow.Right - initial_info.srWindow.Left + 1;
  int rows = initial_info.srWindow.Bottom - initial_info.srWindow.Top + 1;
  core.resize( cols, rows );

  while ( true ) {
    const int timeout = core.tick();
    if ( core.is_finished() ) {
      break;
    }

    CONSOLE_SCREEN_BUFFER_INFO info;
    if ( !GetConsoleScreenBufferInfo( cs.h_out, &info ) ) {
      throw_last_error( "GetConsoleScreenBufferInfo" );
    }
    const int new_cols = info.srWindow.Right - info.srWindow.Left + 1;
    const int new_rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    if ( new_cols != cols || new_rows != rows ) {
      cols = new_cols;
      rows = new_rows;
      core.resize( cols, rows );
    }

    socket_events.reconcile( core.socket_fds() );
    const std::map<intptr_t, WSAEVENT> &registered = socket_events.all();
    if ( registered.size() + 2 > MAXIMUM_WAIT_OBJECTS ) {
      throw ConsoleError( ERROR_TOO_MANY_OPEN_FILES, "too many handles for WaitForMultipleObjects" );
    }

    std::vector<HANDLE> handles;
    std::vector<intptr_t> fds;
    handles.push_back( reader.event() );
    handles.push_back( termination_guard.handle() );
    for ( std::map<intptr_t, WSAEVENT>::const_iterator it = registered.begin(); it != registered.end(); ++it ) {
      handles.push_back( it->second );
      fds.push_back( it->first );
    }
    const DWORD wait_timeout = static_cast<DWORD>( std::max( 0, std::min( timeout, static_cast<int>( RESIZE_POLL_CAP_MS ) ) ) );
    const DWORD result = WaitForMultipleObjects( static_cast<DWORD>( handles.size() ), &handles[0], FALSE, wait_timeout );
    if ( result == WAIT_FAILED ) {
      throw_last_error( "WaitForMultipleObjects" );
    }
    if ( result != WAIT_TIMEOUT && ( result < WAIT_OBJECT_0 || result >= WAIT_OBJECT_0 + handles.size() ) ) {
      throw ConsoleError( ERROR_INVALID_HANDLE, "WaitForMultipleObjects returned an invalid result" );
    }

    reader.check_failure();
    if ( result == WAIT_OBJECT_0 ) {
      const std::string bytes = reader.take_bytes( INPUT_BUDGET_BYTES );
      if ( !bytes.empty() ) {
        core.feed_input( bytes.data(), bytes.size() );
      }
    } else if ( result == WAIT_OBJECT_0 + 1 ) {
      break;
    } else if ( result != WAIT_TIMEOUT ) {
      const size_t index = result - WAIT_OBJECT_0 - 2;
      const intptr_t fd = fds[index];
      WSANETWORKEVENTS events;
      std::memset( &events, 0, sizeof events );
      if ( WSAEnumNetworkEvents( static_cast<SOCKET>( fd ), handles[index + 2], &events ) == SOCKET_ERROR ) {
        const int error = WSAGetLastError();
        throw ConsoleError( static_cast<DWORD>( error ), error_message( "WSAEnumNetworkEvents", error ) );
      }
      if ( events.lNetworkEvents & FD_READ ) {
        const int error = events.iErrorCode[FD_READ_BIT];
        if ( error != 0 ) {
          throw ConsoleError( static_cast<DWORD>( error ), error_message( "FD_READ", error ) );
        }
        core.on_readable( fd );
      }
    }

    const std::string &frame = core.next_frame();
    if ( !frame.empty() ) {
      write_all( cs.h_out, frame );
    }
  }
}
