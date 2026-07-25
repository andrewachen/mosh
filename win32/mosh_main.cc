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

/* ABOUTME: Native Windows mosh.exe entry point and terminal lifecycle owner. */
/* ABOUTME: Restores the console before reporting any session or frontend failure. */

#include "win32/console_io.h"
#include "win32/mosh_bootstrap.h"
#include "src/network/network.h"

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
const int USAGE_EXIT_CODE = 2;
const int FRONTEND_FAILURE_EXIT_CODE = 3;
const int EXCEPTION_EXIT_CODE = 4;

class TerminalGuard {
private:
  const ConsoleState &state;
  const MoshCore &core;

public:
  bool open_started;

  TerminalGuard( const ConsoleState &cs, const MoshCore &session_core )
    : state( cs ), core( session_core ), open_started( false ) {}

  ~TerminalGuard()
  {
    if ( open_started ) {
      try {
        write_all( state.h_out, core.close_sequence() );
      } catch ( ... ) {
      }
    }
    console_raw_restore( &state );
  }
};

bool set_environment( const char *name, const char *value, std::string *message )
{
  if ( !SetEnvironmentVariableA( name, value ) ) {
    *message = std::string( "SetEnvironmentVariableA failed for " ) + name;
    return false;
  }
  return true;
}

std::string bundled_terminfo_path()
{
  char executable[MAX_PATH];
  const DWORD length = GetModuleFileNameA( NULL, executable, sizeof executable );
  if ( length == 0 || length >= sizeof executable ) {
    return "terminfo";
  }
  std::string path( executable, length );
  const std::string::size_type slash = path.find_last_of( "\\/" );
  return slash == std::string::npos ? "terminfo" : path.substr( 0, slash + 1 ) + "terminfo";
}

void print_usage( const char *program )
{
  std::fprintf( stderr,
    "Usage: %s <user@host>                         (connect via ssh)\n"
    "       %s <ip> <port> <key> [predict]         (developer/raw endpoint)\n",
    program, program );
}

}  // namespace

void scrub_key( std::string *key )   // idempotent
{
  if ( key && !key->empty() ) { SecureZeroMemory( &(*key)[0], key->size() ); key->clear(); }
}

int run_console_session( const char *ip, const char *port, std::string *key,
                         const char *predict, int cols, int rows, std::string *message )
{
  int session_rc = 1;
  try {
    MoshCore core( ip, port, key->c_str(), cols, rows, predict );
    scrub_key( key );                          // base64 key consumed by the ctor
    ConsoleState state;
    console_raw_enter( &state );
    {
      TerminalGuard terminal( state, core );
      terminal.open_started = true;
      write_all( state.h_out, core.open_sequence() );
      console_run( core, state );
      session_rc = core.exited_cleanly() ? 0 : 1;
      *message = core.status_message();
    }
  } catch ( const ConsoleError &error ) {
    scrub_key( key ); session_rc = FRONTEND_FAILURE_EXIT_CODE; *message = error.what();
  } catch ( const Network::NetworkException &error ) {
    scrub_key( key ); session_rc = EXCEPTION_EXIT_CODE; *message = error.what();
  } catch ( const std::exception &error ) {
    scrub_key( key ); session_rc = EXCEPTION_EXIT_CODE; *message = error.what();
  }
  return session_rc;
}

int main( int argc, char *argv[] )
{
  const Invocation mode = classify_invocation( argc, argv );
  if ( mode == Invocation::Usage ) { print_usage( argv[0] ); return USAGE_EXIT_CODE; }

  try {
    /* existing setlocale(".UTF-8") + TERM/TERMINFO setup */
    if ( setlocale( LC_ALL, ".UTF-8" ) == NULL ) {
      std::fprintf( stderr, "Unable to configure the UTF-8 locale\n" );
      return EXCEPTION_EXIT_CODE;
    }

    std::string message;
    if ( !set_environment( "TERM", "xterm-256color", &message )
         || !set_environment( "TERMINFO", bundled_terminfo_path().c_str(), &message ) ) {
      std::fprintf( stderr, "%s\n", message.c_str() );
      return EXCEPTION_EXIT_CODE;
    }

    int cols = 0, rows = 0;
    console_dims( &cols, &rows );                 // preflight before spawning ssh

    int session_rc;
    if ( mode == Invocation::Bootstrap ) {
      BootstrapResult ep;
      const std::string err = mosh_bootstrap( argv[1], &ep );
      if ( !err.empty() ) { std::fprintf( stderr, "%s\n", err.c_str() ); return FRONTEND_FAILURE_EXIT_CODE; }
      session_rc = run_console_session( ep.ip.c_str(), ep.port.c_str(), &ep.key, "adaptive", cols, rows, &message );
    } else {
      const char *predict = argc == 5 ? argv[4] : "adaptive";
      std::string devkey( argv[3] );
      session_rc = run_console_session( argv[1], argv[2], &devkey, predict, cols, rows, &message );
    }
    if ( !message.empty() ) std::fprintf( stderr, "%s\n", message.c_str() );
    return session_rc;
  } catch ( const ConsoleError &error ) {
    std::fprintf( stderr, "%s\n", error.what() ); return FRONTEND_FAILURE_EXIT_CODE;
  } catch ( const Network::NetworkException &error ) {
    std::fprintf( stderr, "%s\n", error.what() ); return EXCEPTION_EXIT_CODE;
  } catch ( const std::exception &error ) {
    std::fprintf( stderr, "%s\n", error.what() ); return EXCEPTION_EXIT_CODE;
  }
}
