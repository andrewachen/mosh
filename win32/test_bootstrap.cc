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

/* ABOUTME: Test target for Task 1 bootstrap pure functions (sh_quote, build_remote_command, win_quote_arg). */
/* ABOUTME: Exercises argument quoting and remote-command assembly for the SSH bootstrap. */

#include "win32/mosh_bootstrap.h"
#include "win32/wincompat.h"     /* mosh_winsock_init (used by later tasks) */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

static void test_sh_quote()
{
  assert( sh_quote( "new" ) == "'new'" );
  assert( sh_quote( "LC_ALL=C.UTF-8" ) == "'LC_ALL=C.UTF-8'" );
  assert( sh_quote( "a'b" ) == "'a'\\''b'" );
  assert( sh_quote( "" ) == "''" );
}

static void test_build_remote_command()
{
  const std::string expected =
    "sh -c '[ -n \"$SSH_CONNECTION\" ] && printf \"\\nMOSH SSH_CONNECTION %s\\n\" "
    "\"$SSH_CONNECTION\"' && mosh-server 'new' '-c' '256' '-s' '-l' 'LC_ALL=C.UTF-8'";
  assert( build_remote_command() == expected );
}

static void test_win_quote_arg()
{
  assert( win_quote_arg( "simple" ) == "simple" );
  assert( win_quote_arg( "has space" ) == "\"has space\"" );
  assert( win_quote_arg( "a\"b" ) == "\"a\\\"b\"" );
  assert( win_quote_arg( "" ) == "\"\"" );
  assert( win_quote_arg( "trail\\\\" ) == "trail\\\\" );  /* no special char: unquoted */
}

int main()
{
  test_sh_quote();
  test_build_remote_command();
  test_win_quote_arg();
  puts( "test_bootstrap: passed" );
  return 0;
}
