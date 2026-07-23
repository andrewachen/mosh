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

/* ABOUTME: Automated Windows-console regression probe for reader-thread VT input and poll-driven resize. */
/* ABOUTME: Injects console records so the ARM64 CI runner validates the mechanism without human input. */

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

const DWORD kInputDeadlineMilliseconds = 5000;
const DWORD kJoinDeadlineMilliseconds = 5000;
const DWORD kReaderBufferSize = 4096;
const size_t kPasteChunkSize = 256;

struct ConsoleHandles {
  HANDLE input;
  HANDLE output;
  bool allocated;
  bool owns_handles;
};

struct SavedConsoleState {
  DWORD input_mode;
  DWORD output_mode;
  UINT input_code_page;
  UINT output_code_page;
};

struct SavedConsoleDimensions {
  COORD buffer_size;
  SMALL_RECT window;
};

static bool fail_last_error( const char *what )
{
  fprintf( stderr, "console_probe: %s failed (GetLastError=%lu)\n", what,
           static_cast<unsigned long>( GetLastError() ) );
  return false;
}

static bool fail( const char *what )
{
  fprintf( stderr, "console_probe: %s\n", what );
  return false;
}

static bool is_console( HANDLE handle )
{
  DWORD mode;
  return handle != NULL && handle != INVALID_HANDLE_VALUE
    && GetConsoleMode( handle, &mode ) != 0;
}

static HANDLE open_console_device( const wchar_t *name, DWORD access )
{
  return CreateFileW( name, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                      OPEN_EXISTING, 0, NULL );
}

static bool acquire_console( ConsoleHandles *console )
{
  console->input = GetStdHandle( STD_INPUT_HANDLE );
  console->output = GetStdHandle( STD_OUTPUT_HANDLE );
  console->allocated = false;
  console->owns_handles = false;

  if ( is_console( console->input ) && is_console( console->output ) ) {
    printf( "console_probe: using attached standard console handles\n" );
    return true;
  }

  printf( "console_probe: stdin or stdout is redirected/closed; opening a console for injection\n" );
  HANDLE input = open_console_device( L"CONIN$", GENERIC_READ | GENERIC_WRITE );
  HANDLE output = open_console_device( L"CONOUT$", GENERIC_READ | GENERIC_WRITE );
  if ( is_console( input ) && is_console( output ) ) {
    console->input = input;
    console->output = output;
    console->owns_handles = true;
    printf( "console_probe: using existing console devices CONIN$/CONOUT$\n" );
    return true;
  }

  if ( input != INVALID_HANDLE_VALUE ) {
    CloseHandle( input );
  }
  if ( output != INVALID_HANDLE_VALUE ) {
    CloseHandle( output );
  }

  if ( !AllocConsole() ) {
    fprintf( stderr, "console_probe: cannot obtain a console for injection (GetLastError=%lu)\n",
             static_cast<unsigned long>( GetLastError() ) );
    return false;
  }

  input = open_console_device( L"CONIN$", GENERIC_READ | GENERIC_WRITE );
  output = open_console_device( L"CONOUT$", GENERIC_READ | GENERIC_WRITE );
  if ( !is_console( input ) || !is_console( output ) ) {
    fprintf( stderr, "console_probe: AllocConsole succeeded but CONIN$/CONOUT$ are unusable (GetLastError=%lu)\n",
             static_cast<unsigned long>( GetLastError() ) );
    if ( input != INVALID_HANDLE_VALUE ) {
      CloseHandle( input );
    }
    if ( output != INVALID_HANDLE_VALUE ) {
      CloseHandle( output );
    }
    FreeConsole();
    return false;
  }

  console->input = input;
  console->output = output;
  console->allocated = true;
  console->owns_handles = true;
  printf( "console_probe: allocated a private console for injection\n" );
  return true;
}

static void release_console( const ConsoleHandles &console, bool close_handles = true )
{
  if ( !close_handles ) {
    return;
  }
  if ( console.owns_handles ) {
    CloseHandle( console.input );
    CloseHandle( console.output );
  }
  if ( console.allocated ) {
    FreeConsole();
  }
}

static void restore_console_mode( const ConsoleHandles &console, const SavedConsoleState &saved );

static bool enter_raw_vt_mode( const ConsoleHandles &console, SavedConsoleState *saved )
{
  if ( !GetConsoleMode( console.input, &saved->input_mode ) ) {
    return fail_last_error( "GetConsoleMode(input)" );
  }
  if ( !GetConsoleMode( console.output, &saved->output_mode ) ) {
    return fail_last_error( "GetConsoleMode(output)" );
  }
  saved->input_code_page = GetConsoleCP();
  saved->output_code_page = GetConsoleOutputCP();
  if ( saved->input_code_page == 0 || saved->output_code_page == 0 ) {
    return fail_last_error( "GetConsoleCP/GetConsoleOutputCP" );
  }

  DWORD input_mode = saved->input_mode | ENABLE_VIRTUAL_TERMINAL_INPUT
    | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS;
  input_mode &= ~( ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT
                   | ENABLE_QUICK_EDIT_MODE );
  DWORD output_mode = saved->output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING
    | DISABLE_NEWLINE_AUTO_RETURN;

  if ( !SetConsoleMode( console.input, input_mode ) ) {
    return fail_last_error( "SetConsoleMode(input raw VT)" );
  }
  if ( !SetConsoleMode( console.output, output_mode ) ) {
    SetConsoleMode( console.input, saved->input_mode );
    return fail_last_error( "SetConsoleMode(output VT)" );
  }
  if ( !SetConsoleCP( CP_UTF8 ) ) {
    SetConsoleMode( console.output, saved->output_mode );
    SetConsoleMode( console.input, saved->input_mode );
    return fail_last_error( "SetConsoleCP(CP_UTF8)" );
  }
  if ( !SetConsoleOutputCP( CP_UTF8 ) ) {
    SetConsoleCP( saved->input_code_page );
    SetConsoleMode( console.output, saved->output_mode );
    SetConsoleMode( console.input, saved->input_mode );
    return fail_last_error( "SetConsoleOutputCP(CP_UTF8)" );
  }

  DWORD active_input_mode;
  DWORD active_output_mode;
  if ( !GetConsoleMode( console.input, &active_input_mode )
       || !GetConsoleMode( console.output, &active_output_mode ) ) {
    restore_console_mode( console, *saved );
    return fail_last_error( "GetConsoleMode(verify raw VT mode)" );
  }
  const DWORD required_input = ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT;
  const DWORD forbidden_input = ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT;
  const DWORD required_output = ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
  if ( ( active_input_mode & required_input ) != required_input
       || ( active_input_mode & forbidden_input ) != 0
       || ( active_output_mode & required_output ) != required_output ) {
    restore_console_mode( console, *saved );
    return fail( "console did not retain the requested raw VT mode flags" );
  }
  return true;
}

static void restore_console_mode( const ConsoleHandles &console, const SavedConsoleState &saved )
{
  SetConsoleOutputCP( saved.output_code_page );
  SetConsoleCP( saved.input_code_page );
  SetConsoleMode( console.output, saved.output_mode );
  SetConsoleMode( console.input, saved.input_mode );
}

class ReaderThread {
public:
  explicit ReaderThread( HANDLE input )
    : input_( input ), ready_event_( NULL ), blocking_event_( NULL ), thread_( NULL ),
      stop_( 0 ), in_read_( 0 ), cancellation_issued_( 0 ), error_( ERROR_SUCCESS )
  {
    InitializeCriticalSection( &lock_ );
  }

  ~ReaderThread()
  {
    /* Callers delete a ReaderThread only after joined() is true. If a reader
       cannot be joined, the caller deliberately leaks the whole object. */
    if ( ready_event_ != NULL ) {
      CloseHandle( ready_event_ );
    }
    if ( blocking_event_ != NULL ) {
      CloseHandle( blocking_event_ );
    }
    DeleteCriticalSection( &lock_ );
  }

  bool joined() const
  {
    return thread_ == NULL;
  }

  bool start()
  {
    ready_event_ = CreateEventW( NULL, FALSE, FALSE, NULL );
    if ( ready_event_ == NULL ) {
      return fail_last_error( "CreateEventW(byte-ready)" );
    }
    blocking_event_ = CreateEventW( NULL, TRUE, FALSE, NULL );
    if ( blocking_event_ == NULL ) {
      return fail_last_error( "CreateEventW(blocking read)" );
    }
    thread_ = CreateThread( NULL, 0, thread_main, this, 0, NULL );
    if ( thread_ == NULL ) {
      return fail_last_error( "CreateThread(console reader)" );
    }
    return true;
  }

  HANDLE ready_event() const
  {
    return ready_event_;
  }

  bool wait_until_blocked()
  {
    if ( InterlockedCompareExchange( &in_read_, 0, 0 ) != 0 ) {
      return true;
    }
    ResetEvent( blocking_event_ );
    if ( InterlockedCompareExchange( &in_read_, 0, 0 ) != 0 ) {
      return true;
    }

    DWORD result = WaitForSingleObject( blocking_event_, kInputDeadlineMilliseconds );
    if ( result == WAIT_OBJECT_0 ) {
      return true;
    }
    if ( result == WAIT_FAILED ) {
      return fail_last_error( "WaitForSingleObject(blocking read)" );
    }
    return fail( "reader did not reach a blocking ReadFile call" );
  }

  bool cancellation_issued()
  {
    return InterlockedCompareExchange( &cancellation_issued_, 0, 0 ) != 0;
  }

  std::string take_bytes()
  {
    EnterCriticalSection( &lock_ );
    std::string bytes;
    bytes.swap( queue_ );
    LeaveCriticalSection( &lock_ );
    return bytes;
  }

  DWORD error() const
  {
    EnterCriticalSection( &lock_ );
    DWORD error = error_;
    LeaveCriticalSection( &lock_ );
    return error;
  }

  bool stop()
  {
    if ( thread_ == NULL ) {
      return true;
    }
    if ( !wait_until_blocked() ) {
      return false;
    }

    InterlockedExchange( &stop_, 1 );
    ULONGLONG deadline = GetTickCount64() + kJoinDeadlineMilliseconds;
    for ( ;; ) {
      if ( CancelSynchronousIo( thread_ ) ) {
        /* Success proves a pending synchronous ReadFile was actually cancelled. */
        InterlockedExchange( &cancellation_issued_, 1 );
        break;
      }
      if ( GetLastError() != ERROR_NOT_FOUND ) {
        return fail_last_error( "CancelSynchronousIo(console reader)" );
      }
      if ( GetTickCount64() >= deadline ) {
        return fail( "reader never reached a cancellable ReadFile call" );
      }
      Sleep( 1 );
    }

    for ( ;; ) {
      DWORD result = WaitForSingleObject( thread_, 50 );
      if ( result == WAIT_OBJECT_0 ) {
        CloseHandle( thread_ );
        thread_ = NULL;
        return true;
      }
      if ( result == WAIT_FAILED ) {
        return fail_last_error( "WaitForSingleObject(console reader)" );
      }
      if ( result != WAIT_TIMEOUT ) {
        fprintf( stderr, "console_probe: unexpected reader join result %lu\n",
                 static_cast<unsigned long>( result ) );
        return false;
      }
      if ( GetTickCount64() >= deadline ) {
        return fail( "reader thread did not exit after cancellation" );
      }
    }
  }

private:
  static DWORD WINAPI thread_main( LPVOID parameter )
  {
    ReaderThread *reader = static_cast<ReaderThread *>( parameter );
    char buffer[kReaderBufferSize];

    while ( InterlockedCompareExchange( &reader->stop_, 0, 0 ) == 0 ) {
      InterlockedExchange( &reader->in_read_, 1 );
      SetEvent( reader->blocking_event_ );
      DWORD bytes_read = 0;
      if ( ReadFile( reader->input_, buffer, sizeof( buffer ), &bytes_read, NULL ) ) {
        InterlockedExchange( &reader->in_read_, 0 );
        if ( bytes_read == 0 ) {
          reader->record_error( ERROR_READ_FAULT );
          return 0;
        }
        reader->append_bytes( buffer, bytes_read );
        continue;
      }

      InterlockedExchange( &reader->in_read_, 0 );
      DWORD error = GetLastError();
      if ( InterlockedCompareExchange( &reader->stop_, 0, 0 ) != 0
           && error == ERROR_OPERATION_ABORTED ) {
        return 0;
      }
      reader->record_error( error );
      return 0;
    }
    return 0;
  }

  void append_bytes( const char *bytes, DWORD count )
  {
    EnterCriticalSection( &lock_ );
    queue_.append( bytes, count );
    LeaveCriticalSection( &lock_ );
    SetEvent( ready_event_ );
  }

  void record_error( DWORD error )
  {
    EnterCriticalSection( &lock_ );
    error_ = error;
    LeaveCriticalSection( &lock_ );
    SetEvent( ready_event_ );
  }

  HANDLE input_;
  HANDLE ready_event_;
  HANDLE blocking_event_;
  HANDLE thread_;
  volatile LONG stop_;
  volatile LONG in_read_;
  volatile LONG cancellation_issued_;
  mutable CRITICAL_SECTION lock_;
  std::string queue_;
  DWORD error_;
};

static INPUT_RECORD make_key( WCHAR character, DWORD control_key_state = 0,
                              WORD virtual_key = 0 )
{
  INPUT_RECORD record = {};
  record.EventType = KEY_EVENT;
  record.Event.KeyEvent.bKeyDown = TRUE;
  record.Event.KeyEvent.wRepeatCount = 1;
  /* A char-only injected record has no physical virtual-key identity. Leave
     VK/scan code zero so conhost translates its UnicodeChar, not a bogus VK. */
  record.Event.KeyEvent.wVirtualKeyCode = virtual_key;
  record.Event.KeyEvent.wVirtualScanCode = 0;
  record.Event.KeyEvent.uChar.UnicodeChar = character;
  record.Event.KeyEvent.dwControlKeyState = control_key_state;
  return record;
}

static INPUT_RECORD make_resize_event( SHORT columns, SHORT rows )
{
  INPUT_RECORD record = {};
  record.EventType = WINDOW_BUFFER_SIZE_EVENT;
  record.Event.WindowBufferSizeEvent.dwSize = { columns, rows };
  return record;
}

static bool inject( HANDLE input, const std::vector<INPUT_RECORD> &records )
{
  DWORD written = 0;
  if ( !WriteConsoleInputW( input, records.data(), static_cast<DWORD>( records.size() ), &written ) ) {
    return fail_last_error( "WriteConsoleInputW" );
  }
  if ( written != records.size() ) {
    fprintf( stderr, "console_probe: WriteConsoleInputW wrote %lu of %zu records\n",
             static_cast<unsigned long>( written ), records.size() );
    return false;
  }
  return true;
}

static bool receive_exact( ReaderThread *reader, const std::string &expected,
                           const char *description )
{
  std::string received;
  ULONGLONG deadline = GetTickCount64() + kInputDeadlineMilliseconds;
  while ( received.size() < expected.size() ) {
    received += reader->take_bytes();
    if ( received.size() >= expected.size() ) {
      break;
    }
    DWORD thread_error = reader->error();
    if ( thread_error != ERROR_SUCCESS ) {
      fprintf( stderr, "console_probe: reader failed while waiting for %s (GetLastError=%lu)\n",
               description, static_cast<unsigned long>( thread_error ) );
      return false;
    }

    ULONGLONG now = GetTickCount64();
    if ( now >= deadline ) {
      fprintf( stderr, "console_probe: timed out waiting for %s (%zu of %zu bytes)\n", description,
               received.size(), expected.size() );
      return false;
    }
    DWORD result = WaitForSingleObject( reader->ready_event(),
                                        static_cast<DWORD>( deadline - now ) );
    if ( result == WAIT_FAILED ) {
      return fail_last_error( "WaitForSingleObject(byte-ready)" );
    }
    if ( result != WAIT_OBJECT_0 ) {
      fprintf( stderr, "console_probe: byte-ready wait timed out while waiting for %s\n", description );
      return false;
    }
  }

  if ( received != expected ) {
    fprintf( stderr, "console_probe: byte mismatch for %s (%zu received, %zu expected)\n", description,
             received.size(), expected.size() );
    return false;
  }
  return true;
}

static void append_key_text( std::vector<INPUT_RECORD> *records, const std::wstring &text )
{
  for ( size_t i = 0; i < text.size(); i++ ) {
    records->push_back( make_key( text[i] ) );
  }
}

static bool test_modifier_does_not_stall( HANDLE input, ReaderThread *reader )
{
  std::vector<INPUT_RECORD> prefix;
  prefix.push_back( make_key( L'a' ) );
  prefix.push_back( make_key( 0, SHIFT_PRESSED, VK_SHIFT ) );
  if ( !inject( input, prefix ) || !receive_exact( reader, "a", "a before trailing Shift-only key" ) ) {
    return false;
  }

  std::vector<INPUT_RECORD> suffix( 1, make_key( L'b' ) );
  if ( !inject( input, suffix ) || !receive_exact( reader, "b", "b after Shift-only key" ) ) {
    return false;
  }
  printf( "console_probe: modifier-only key did not stall reader byte delivery\n" );
  return true;
}

static bool inject_and_receive( HANDLE input, ReaderThread *reader,
                                const std::vector<INPUT_RECORD> &records,
                                const std::string &expected, const char *description )
{
  return inject( input, records ) && receive_exact( reader, expected, description );
}

static bool test_vt_bytes( HANDLE input, ReaderThread *reader )
{
  std::vector<INPUT_RECORD> ascii;
  append_key_text( &ascii, L"ascii:" );
  if ( !inject_and_receive( input, reader, ascii, "ascii:", "ASCII characters" ) ) {
    return false;
  }

  std::vector<INPUT_RECORD> e_acute( 1, make_key( 0x00e9 ) );
  if ( !inject_and_receive( input, reader, e_acute, "\xc3\xa9", "U+00E9 UTF-8" ) ) {
    return false;
  }

  std::vector<INPUT_RECORD> emoji;
  emoji.push_back( make_key( 0xd83d ) );
  emoji.push_back( make_key( 0xde00 ) );
  /* This surrogate pair -> four UTF-8 byte expectation is the assertion most
     likely to require adjustment after the first real conhost CI observation. */
  if ( !inject_and_receive( input, reader, emoji, "\xf0\x9f\x98\x80", "surrogate-pair emoji" ) ) {
    return false;
  }

  std::vector<INPUT_RECORD> ctrl_a( 1, make_key( 0x0001, LEFT_CTRL_PRESSED, L'A' ) );
  if ( !inject_and_receive( input, reader, ctrl_a, "\x01", "Ctrl-A" ) ) {
    return false;
  }

  std::vector<INPUT_RECORD> alt_x( 1, make_key( L'x', LEFT_ALT_PRESSED, L'X' ) );
  if ( !inject_and_receive( input, reader, alt_x, "\x1bx", "Alt-X" ) ) {
    return false;
  }

  std::vector<INPUT_RECORD> up_arrow( 1, make_key( 0, 0, VK_UP ) );
  if ( !inject_and_receive( input, reader, up_arrow, "\x1b[A", "Up arrow" ) ) {
    return false;
  }

  const size_t total_paste_size = 8192;
  for ( size_t written = 0; written < total_paste_size; written += kPasteChunkSize ) {
    size_t chunk_size = std::min( kPasteChunkSize, total_paste_size - written );
    std::vector<INPUT_RECORD> chunk( chunk_size, make_key( L'p' ) );
    if ( !inject( input, chunk ) || !receive_exact( reader, std::string( chunk_size, 'p' ),
                                                     "incremental large paste" ) ) {
      return false;
    }
  }

  printf( "console_probe: reader delivered UTF-8, modifiers, arrow VT bytes, and an incremental 8192-byte paste\n" );
  return true;
}

static bool poll_dimensions( HANDLE output, COORD *dimensions )
{
  CONSOLE_SCREEN_BUFFER_INFO info;
  if ( !GetConsoleScreenBufferInfo( output, &info ) ) {
    return fail_last_error( "GetConsoleScreenBufferInfo" );
  }
  dimensions->X = static_cast<SHORT>( info.srWindow.Right - info.srWindow.Left + 1 );
  dimensions->Y = static_cast<SHORT>( info.srWindow.Bottom - info.srWindow.Top + 1 );
  return true;
}

static bool save_dimensions( HANDLE output, SavedConsoleDimensions *saved )
{
  CONSOLE_SCREEN_BUFFER_INFO info;
  if ( !GetConsoleScreenBufferInfo( output, &info ) ) {
    return fail_last_error( "GetConsoleScreenBufferInfo(save dimensions)" );
  }
  saved->buffer_size = info.dwSize;
  saved->window = info.srWindow;
  return true;
}

static bool set_window_size( HANDLE output, SHORT columns, SHORT rows )
{
  CONSOLE_SCREEN_BUFFER_INFO info;
  if ( !GetConsoleScreenBufferInfo( output, &info ) ) {
    return fail_last_error( "GetConsoleScreenBufferInfo(before resize)" );
  }

  SHORT current_columns = static_cast<SHORT>( info.srWindow.Right - info.srWindow.Left + 1 );
  SHORT current_rows = static_cast<SHORT>( info.srWindow.Bottom - info.srWindow.Top + 1 );
  SMALL_RECT shrink = info.srWindow;
  shrink.Right = static_cast<SHORT>( shrink.Left + std::min( current_columns, columns ) - 1 );
  shrink.Bottom = static_cast<SHORT>( shrink.Top + std::min( current_rows, rows ) - 1 );
  if ( !SetConsoleWindowInfo( output, TRUE, &shrink ) ) {
    return fail_last_error( "SetConsoleWindowInfo(shrink before resize)" );
  }

  COORD buffer_size = { columns, rows };
  if ( !SetConsoleScreenBufferSize( output, buffer_size ) ) {
    return fail_last_error( "SetConsoleScreenBufferSize" );
  }
  SMALL_RECT window = { 0, 0, static_cast<SHORT>( columns - 1 ), static_cast<SHORT>( rows - 1 ) };
  if ( !SetConsoleWindowInfo( output, TRUE, &window ) ) {
    return fail_last_error( "SetConsoleWindowInfo(resize)" );
  }
  return true;
}

static bool restore_dimensions( HANDLE output, const SavedConsoleDimensions &saved )
{
  CONSOLE_SCREEN_BUFFER_INFO info;
  if ( !GetConsoleScreenBufferInfo( output, &info ) ) {
    return fail_last_error( "GetConsoleScreenBufferInfo(restore dimensions)" );
  }

  SHORT current_columns = static_cast<SHORT>( info.srWindow.Right - info.srWindow.Left + 1 );
  SHORT current_rows = static_cast<SHORT>( info.srWindow.Bottom - info.srWindow.Top + 1 );
  SMALL_RECT shrink = { 0, 0,
                        static_cast<SHORT>( std::min( current_columns, saved.buffer_size.X ) - 1 ),
                        static_cast<SHORT>( std::min( current_rows, saved.buffer_size.Y ) - 1 ) };
  if ( !SetConsoleWindowInfo( output, TRUE, &shrink ) ) {
    return fail_last_error( "SetConsoleWindowInfo(shrink before restore)" );
  }
  if ( !SetConsoleScreenBufferSize( output, saved.buffer_size ) ) {
    return fail_last_error( "SetConsoleScreenBufferSize(restore)" );
  }
  if ( !SetConsoleWindowInfo( output, TRUE, &saved.window ) ) {
    return fail_last_error( "SetConsoleWindowInfo(restore)" );
  }
  return true;
}

static bool test_poll_resize( HANDLE input, HANDLE output, ReaderThread *reader,
                              bool private_console )
{
  if ( !private_console ) {
    printf( "console_probe: skipped destructive resize on an attached console\n" );
    return true;
  }

  SavedConsoleDimensions saved;
  if ( !save_dimensions( output, &saved ) ) {
    return false;
  }

  COORD before;
  if ( !poll_dimensions( output, &before ) ) {
    return false;
  }
  COORD target = { static_cast<SHORT>( before.X == 80 ? 81 : 80 ),
                   static_cast<SHORT>( before.Y == 25 ? 26 : 25 ) };

  bool changed = set_window_size( output, target.X, target.Y );
  COORD observed = {};
  if ( changed ) {
    changed = poll_dimensions( output, &observed ) && observed.X == target.X && observed.Y == target.Y;
  }
  if ( !changed ) {
    restore_dimensions( output, saved );
    fprintf( stderr, "console_probe: poll did not observe requested resize %d,%d\n", target.X, target.Y );
    return false;
  }

  std::vector<INPUT_RECORD> records;
  records.push_back( make_resize_event( 1, 1 ) );
  records.push_back( make_key( L'z' ) );
  bool passed = inject( input, records ) && receive_exact( reader, "z", "key after injected resize record" );
  if ( passed ) {
    passed = poll_dimensions( output, &observed ) && observed.X == target.X && observed.Y == target.Y;
  }
  bool restored = restore_dimensions( output, saved );
  if ( !restored ) {
    return false;
  }
  if ( !passed ) {
    return fail( "injected resize record affected poll-driven dimensions" );
  }

  printf( "console_probe: polled actual resize and ignored an injected WINDOW_BUFFER_SIZE_EVENT\n" );
  return true;
}

} /* namespace */

int main()
{
  ConsoleHandles console;
  if ( !acquire_console( &console ) ) {
    return 1;
  }

  SavedConsoleState saved;
  bool entered_raw_mode = enter_raw_vt_mode( console, &saved );
  bool passed = false;
  bool reader_joined = true;
  if ( entered_raw_mode ) {
    if ( !FlushConsoleInputBuffer( console.input ) ) {
      fail_last_error( "FlushConsoleInputBuffer" );
    } else {
      ReaderThread *reader = new ReaderThread( console.input );
      if ( reader->start() ) {
        passed = test_modifier_does_not_stall( console.input, reader )
          && test_vt_bytes( console.input, reader )
          && test_poll_resize( console.input, console.output, reader, console.allocated );
        if ( !reader->stop() ) {
          passed = false;
        }
        if ( !reader->cancellation_issued() ) {
          fail( "reader cancellation was not issued" );
          passed = false;
        }
        reader_joined = reader->joined();
        if ( reader->error() != ERROR_SUCCESS ) {
          fprintf( stderr, "console_probe: reader exited with GetLastError=%lu\n",
                   static_cast<unsigned long>( reader->error() ) );
          passed = false;
        }
      }
      if ( reader->joined() ) {
        delete reader;
      } else {
        /* Keep every resource alive while the unjoinable reader may use it. */
        reader_joined = false;
      }
    }
    restore_console_mode( console, saved );
  }
  release_console( console, reader_joined );

  if ( !passed ) {
    return 1;
  }
  printf( "console_probe: PASS\n" );
  return 0;
}
