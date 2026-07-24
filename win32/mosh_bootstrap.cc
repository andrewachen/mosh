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

/* ABOUTME: Implementation of pure bootstrap helpers: sh_quote, build_remote_command, win_quote_arg. */
/* ABOUTME: Task 1 of M3 (SSH bootstrap) milestone - pure functions for argument quoting and remote-command assembly. */

#include "win32/mosh_bootstrap.h"

#include <string>

std::string sh_quote( const std::string &arg )
{
  std::string out = "'";
  for ( char c : arg ) { if ( c == '\'' ) out += "'\\''"; else out += c; }
  out += "'";
  return out;
}

std::string build_remote_command()
{
  /* mosh.pl:367-369 probe; \n are literal backslash-n for the remote printf. */
  const std::string probe =
    "[ -n \"$SSH_CONNECTION\" ] && "
    "printf \"\\nMOSH SSH_CONNECTION %s\\n\" \"$SSH_CONNECTION\"";
  /* mosh.pl:377-389 minimal server args; no trailing -- (no remote command). */
  const char *server_args[] = { "new", "-c", "256", "-s", "-l", "LC_ALL=C.UTF-8" };
  /* '&&' (not upstream's ';'): if there is no SSH_CONNECTION the probe fails and
     mosh-server never starts, avoiding an orphaned detached server. */
  std::string cmd = "sh -c " + sh_quote( probe ) + " && mosh-server";
  for ( const char *a : server_args ) cmd += " " + sh_quote( a );
  return cmd;
}

std::string win_quote_arg( const std::string &arg )
{
  if ( !arg.empty() && arg.find_first_of( " \t\n\v\"" ) == std::string::npos ) return arg;
  std::string out = "\"";
  for ( std::string::const_iterator it = arg.begin(); ; ++it ) {
    unsigned bs = 0;
    while ( it != arg.end() && *it == '\\' ) { ++it; ++bs; }
    if ( it == arg.end() ) { out.append( bs * 2, '\\' ); break; }
    else if ( *it == '"' ) { out.append( bs * 2 + 1, '\\' ); out += '"'; }
    else { out.append( bs, '\\' ); out += *it; }
  }
  out += "\"";
  return out;
}
