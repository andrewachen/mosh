/* ABOUTME: Tests the Windows exit diagnostics selected after a mosh session. */
/* ABOUTME: Verifies connection failure, unclean shutdown, clean shutdown, and the exit banner. */
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
    file(s), but you are not obligated to do so. If you do not wish
    to do so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

#include <cstdio>
#include <string>

#include "win32/exit_diagnostics.h"
#include "win32/upstream_strings.h"

static int test_never_connected()
{
  const std::string text = exit_diagnostic( true, false, "203.0.113.7", "60001" );
  if ( text.find( "\nmosh did not make a successful connection to 203.0.113.7:60001.\n"
                  "Please verify that UDP port 60001 is not firewalled and can reach the server.\n\n" )
       == std::string::npos ) {
    fprintf( stderr, "FAIL: never-connected diagnostic did not name the endpoint\n" );
    return 1;
  }
  if ( text.find( "-p" ) != std::string::npos ) {
    fprintf( stderr, "FAIL: never-connected diagnostic mentioned unsupported -p\n" );
    return 1;
  }
  return 0;
}

static int test_connected_unclean()
{
  const std::string text = exit_diagnostic( false, false, "203.0.113.7", "60001" );
  if ( text
       != "\n\nmosh did not shut down cleanly. Please note that the\n"
          "mosh-server process may still be running on the server.\n" ) {
    fprintf( stderr, "FAIL: connected-unclean diagnostic did not match upstream\n" );
    return 1;
  }
  return 0;
}

static int test_clean_exit()
{
  if ( !exit_diagnostic( false, true, "203.0.113.7", "60001" ).empty() ) {
    fprintf( stderr, "FAIL: clean exit emitted a diagnostic\n" );
    return 1;
  }
  return 0;
}

static int test_banner()
{
  if ( std::string( EXIT_BANNER ) != "[mosh is exiting.]\n" ) {
    fprintf( stderr, "FAIL: exit banner did not match upstream\n" );
    return 1;
  }
  return 0;
}

int main()
{
  if ( test_never_connected() != 0 || test_connected_unclean() != 0 || test_clean_exit() != 0
       || test_banner() != 0 ) {
    return 1;
  }
  printf( "test_exit_diagnostics: all cases passed\n" );
  return 0;
}
