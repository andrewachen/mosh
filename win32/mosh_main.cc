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
  std::fprintf( stderr, "Usage: %s <ip> <port> <key> [predict]\n", program );
}

bool valid_predict( const char *predict )
{
  return std::string( predict ) == "adaptive" || std::string( predict ) == "always"
    || std::string( predict ) == "never" || std::string( predict ) == "experimental";
}
}

int main( int argc, char *argv[] )
{
  if ( ( argc != 4 && argc != 5 ) || ( argc == 5 && !valid_predict( argv[4] ) ) ) {
    print_usage( argv[0] );
    return USAGE_EXIT_CODE;
  }

  try {
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

    int cols = 0;
    int rows = 0;
    console_dims( &cols, &rows );
    const char *predict = argc == 5 ? argv[4] : "adaptive";
    MoshCore core( argv[1], argv[2], argv[3], cols, rows, predict );

    int session_rc = 1;
    try {
      ConsoleState state;
      console_raw_enter( &state );
      {
        TerminalGuard terminal( state, core );
        terminal.open_started = true;
        write_all( state.h_out, core.open_sequence() );
        console_run( core, state );
        session_rc = core.exited_cleanly() ? 0 : 1;
        message = core.status_message();
      }
    } catch ( const ConsoleError &error ) {
      session_rc = FRONTEND_FAILURE_EXIT_CODE;
      message = error.what();
    } catch ( const Network::NetworkException &error ) {
      session_rc = EXCEPTION_EXIT_CODE;
      message = error.what();
    } catch ( const std::exception &error ) {
      session_rc = EXCEPTION_EXIT_CODE;
      message = error.what();
    }

    if ( !message.empty() ) {
      std::fprintf( stderr, "%s\n", message.c_str() );
    }
    return session_rc;
  } catch ( const ConsoleError &error ) {
    std::fprintf( stderr, "%s\n", error.what() );
    return FRONTEND_FAILURE_EXIT_CODE;
  } catch ( const Network::NetworkException &error ) {
    std::fprintf( stderr, "%s\n", error.what() );
    return EXCEPTION_EXIT_CODE;
  } catch ( const std::exception &error ) {
    std::fprintf( stderr, "%s\n", error.what() );
    return EXCEPTION_EXIT_CODE;
  }
}
