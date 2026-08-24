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
#include "win32/exit_diagnostics.h"
#include "win32/mosh_bootstrap.h"
#include "win32/upstream_strings.h"
#include "win32/startup_options.h"
#include "src/network/network.h"

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
const int USAGE_EXIT_CODE = 2;
const int FRONTEND_FAILURE_EXIT_CODE = 3;
const int EXCEPTION_EXIT_CODE = 4;

/* Names the first restore failure, if any, for the stderr message. Keeps the
   mapping from CleanupOp to a human-readable name out of the session so a
   renumbered enum cannot silently mislabel an error here. */
const char *cleanup_op_name( int op )
{
  switch ( op ) {
    case CLEANUP_OP_NONE:
      return "none";
    case CLEANUP_OP_CLOSE_SEQUENCE:
      return "writing the close sequence";
    case CLEANUP_OP_OUTPUT_CP:
      return "restoring the output code page";
    case CLEANUP_OP_INPUT_CP:
      return "restoring the input code page";
    case CLEANUP_OP_OUTPUT_MODE:
      return "restoring the output mode";
    case CLEANUP_OP_INPUT_MODE:
      return "restoring the input mode";
    default:
      return "an unknown step";
  }
}

void print_usage( const char *program )
{
  std::fprintf( stderr, "Usage: %s [-v ...] <user@host>  (connect via ssh)\n", program );
}

}  // namespace

void scrub_key( std::string *key )   // idempotent
{
  if ( key && !key->empty() ) { SecureZeroMemory( &(*key)[0], key->size() ); key->clear(); }
}

int run_console_session( const char *ip, const char *port, std::string *key,
                         const StartupOptions &opts, int cols, int rows, std::string *message,
                         CleanupReport *cleanup, bool *have_exit_facts, bool *never_connected,
                         bool *clean_shutdown )
{
  int session_rc = 1;
  *have_exit_facts = false;
  *never_connected = false;
  *clean_shutdown = false;
  try {
    MoshCore core( ip, port, key->c_str(), cols, rows, opts );
    scrub_key( key );                          // base64 key consumed by the ctor
    /* The constructor performs every console mutation and writes the open
       sequence; by the time it returns the session is fully live. */
    ConsoleSession session( core );
    *have_exit_facts = true;
    *never_connected = core.still_connecting();
    try {
      session.run();
    } catch ( ... ) {
      /* run() restores before propagating, so the report is available
         here whether or not it threw. */
      *cleanup = session.cleanup_report();
      *never_connected = core.still_connecting();
      *clean_shutdown = core.exited_cleanly();
      throw;
    }
    *cleanup = session.cleanup_report();
    *never_connected = core.still_connecting();
    *clean_shutdown = core.exited_cleanly();
    session_rc = *clean_shutdown ? 0 : 1;
    *message = core.status_message();
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
  unsigned verbose = 0;
  const char *destination = nullptr;
  if ( !parse_invocation( argc, argv, &verbose, &destination ) ) { print_usage( argv[0] ); return USAGE_EXIT_CODE; }

  try {
    if ( setlocale( LC_ALL, ".UTF-8" ) == NULL ) {
      std::fprintf( stderr, "Unable to configure the UTF-8 locale\n" );
      return EXCEPTION_EXIT_CODE;
    }

    int cols = 0, rows = 0;
    console_dims( &cols, &rows );                 // preflight before spawning ssh

    StartupEnv env;
    env.predict_display = std::getenv( "MOSH_PREDICTION_DISPLAY" );
    env.predict_overwrite = std::getenv( "MOSH_PREDICTION_OVERWRITE" );
    env.escape_key = std::getenv( "MOSH_ESCAPE_KEY" );
    env.title_noprefix = std::getenv( "MOSH_TITLE_NOPREFIX" );
    env.no_term_init = std::getenv( "MOSH_NO_TERM_INIT" );
    StartupOptions opts;
    std::string opt_error;
    if ( !parse_startup_options( env, &opts, &opt_error ) ) {
      std::fprintf( stderr, "%s\n", opt_error.c_str() );
      return USAGE_EXIT_CODE;
    }
    opts.verbose = verbose;

    CleanupReport cleanup = { ERROR_SUCCESS, CLEANUP_OP_NONE, ERROR_SUCCESS };
    BootstrapResult ep;
    const std::string err = mosh_bootstrap( destination, &ep );
    if ( !err.empty() ) { std::fprintf( stderr, "%s\n", err.c_str() ); return FRONTEND_FAILURE_EXIT_CODE; }
    std::string message;
    bool have_exit_facts = false;
    bool never_connected = false;
    bool clean_shutdown = false;
    const int session_rc = run_console_session( ep.ip.c_str(), ep.port.c_str(), &ep.key, opts, cols, rows,
                                                &message, &cleanup, &have_exit_facts, &never_connected,
                                                &clean_shutdown );
    if ( !message.empty() ) {
      std::fprintf( stderr, "%s\n", message.c_str() );
    }
    if ( cleanup.reader_error != ERROR_SUCCESS ) {
      std::fprintf( stderr, "ReadFile console input (error %lu)\n", cleanup.reader_error );
    }
    if ( cleanup.failed_op != CLEANUP_OP_NONE ) {
      std::fprintf( stderr, "warning: %s failed while restoring the console (GetLastError=%lu)\n",
                    cleanup_op_name( cleanup.failed_op ), cleanup.first_error );
    }
    if ( have_exit_facts ) {
      const std::string diagnostic = exit_diagnostic( never_connected, clean_shutdown,
                                                      ep.ip.c_str(), ep.port.c_str() );
      if ( !diagnostic.empty() ) {
        std::fputs( diagnostic.c_str(), stderr );
      }
    }
    std::fputs( EXIT_BANNER, stdout );
    return session_rc;
  } catch ( const ConsoleError &error ) {
    std::fprintf( stderr, "%s\n", error.what() ); return FRONTEND_FAILURE_EXIT_CODE;
  } catch ( const Network::NetworkException &error ) {
    std::fprintf( stderr, "%s\n", error.what() ); return EXCEPTION_EXIT_CODE;
  } catch ( const std::exception &error ) {
    std::fprintf( stderr, "%s\n", error.what() ); return EXCEPTION_EXIT_CODE;
  }
}
