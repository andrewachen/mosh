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

/* ABOUTME: Acceptance harness for the native Windows console lifecycle (raw enter/restore). */
/* ABOUTME: Drives console_raw_enter/console_raw_restore on the inherited console and asserts output-mode flags. */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "win32/console_io.h"

/* NDEBUG-proof check for load-bearing Win32 setup calls: aborts loudly with the
   last error rather than being compiled out like assert(). */
static void must( BOOL ok, const char *what )
{
  if ( !ok ) {
    fprintf( stderr, "fatal: %s failed (GetLastError=%lu)\n", what,
             GetLastError() );
    abort();
  }
}

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
    must( SetStdHandle( STD_OUTPUT_HANDLE, conout_ ), "SetStdHandle(out)" );
    must( SetStdHandle( STD_INPUT_HANDLE, conin_ ), "SetStdHandle(in)" );
  }
  ~ConsoleTestScope()
  {
    SetConsoleMode( conout_, orig_out_mode_ );
    SetConsoleMode( conin_, orig_in_mode_ );
    SetConsoleOutputCP( orig_out_cp_ );
    SetConsoleCP( orig_in_cp_ );
    SetStdHandle( STD_OUTPUT_HANDLE, orig_out_ );
    SetStdHandle( STD_INPUT_HANDLE, orig_in_ );
    CloseHandle( conout_ );
    CloseHandle( conin_ );
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
};

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

int main( int argc, char *argv[] )
{
  if ( argc < 2 ) {
    fprintf( stderr, "usage: %s <mode>\n", argv[0] );
    return 1;
  }

  if ( strcmp( argv[1], "vt-mode" ) == 0 ) {
    return run_vt_mode();
  }

  fprintf( stderr, "error: unknown mode '%s'\n", argv[1] );
  return 1;
}
