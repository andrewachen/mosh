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
#include <regex>
#include <sstream>
#include <vector>
#include <cstdlib>
#include "win32/wincompat.h"
#include <winsock2.h>
#include <ws2tcpip.h>

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

std::string parse_server_line( const std::string &line, ServerReply *r )
{
  static const std::regex connect_re( "^MOSH CONNECT (\\d+) ([A-Za-z0-9/+]{22})\\s*$" );
  static const std::regex ip_re( "^MOSH IP (\\S+)\\s*$" );
  std::smatch m;

  if ( line.compare( 0, 13, "MOSH CONNECT " ) == 0 ) {
    if ( std::regex_match( line, m, connect_re ) ) {
      r->port = m[1].str(); r->key = m[2].str(); r->have_connect = true;
      return "";
    }
    return "mosh: bad MOSH CONNECT string from server";       // mosh.pl:433
  }
  if ( line.compare( 0, 20, "MOSH SSH_CONNECTION " ) == 0 ) {
    std::istringstream iss( line );
    std::vector<std::string> tok; std::string t;
    while ( iss >> t ) tok.push_back( t );
    if ( tok.size() != 6 ) return "mosh: bad MOSH SSH_CONNECTION string from server";  // mosh.pl:427
    r->sship = tok[4];
    return "";
  }
  if ( line.compare( 0, 8, "MOSH IP " ) == 0 ) {
    if ( !r->mosh_ip.empty() ) return "mosh: server redefined MOSH IP";               // mosh.pl:419
    if ( std::regex_match( line, m, ip_re ) ) { r->mosh_ip = m[1].str(); return ""; }
    return "mosh: bad MOSH IP string from server";
  }
  return "";   // banner / diagnostic -> ignore
}

bool is_numeric_ip( const std::string &s )
{
  if ( s.empty() || s.find( '\0' ) != std::string::npos ) return false;
  unsigned char buf[sizeof( struct in6_addr )];
  return inet_pton( AF_INET, s.c_str(), buf ) == 1 || inet_pton( AF_INET6, s.c_str(), buf ) == 1;
}

std::string resolve_endpoint( const ServerReply &r, const std::string &target, BootstrapResult *out )
{
  if ( !r.have_connect || r.port.empty() || r.key.empty() )
    return "mosh: did not find the mosh server startup message (is mosh installed on " + target + "?)";
  char *end = NULL;
  const long port = std::strtol( r.port.c_str(), &end, 10 );
  if ( end == r.port.c_str() || *end != '\0' || port < 1 || port > 65535 )
    return "mosh: server reported an invalid UDP port (" + r.port + ")";
  const std::string ip = !r.mosh_ip.empty() ? r.mosh_ip : r.sship;   // mosh.pl:445-448
  if ( ip.empty() )
    return "mosh: requires a direct UDP endpoint to " + target + "; proxied SSH is unsupported";
  if ( !is_numeric_ip( ip ) )
    return "mosh: server reported a non-numeric address (" + ip + ")";
  out->ip = ip; out->port = r.port; out->key = r.key;
  return "";
}
