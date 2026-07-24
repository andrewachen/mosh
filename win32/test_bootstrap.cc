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

/* ABOUTME: Test target for Task 1-3 bootstrap functions: sh_quote, build_remote_command, win_quote_arg, drain, spawn. */
/* ABOUTME: Exercises argument quoting, server output parsing, and Win32 spawn with pipe drain. */

#include "win32/mosh_bootstrap.h"
#include "win32/mosh_bootstrap_internal.h"
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

static void test_parse_connect()
{
  ServerReply r;
  assert( parse_server_line( "MOSH CONNECT 60001 ABCDEFGHIJKLMNOPQRSTUV", &r ).empty() );
  assert( r.have_connect && r.port == "60001" && r.key == "ABCDEFGHIJKLMNOPQRSTUV" );
  ServerReply bad;                                    /* malformed CONNECT -> fatal */
  assert( !parse_server_line( "MOSH CONNECT 60001 tooshort", &bad ).empty() );
  assert( !bad.have_connect );
}

static void test_parse_ssh_connection()
{
  ServerReply r;
  assert( parse_server_line( "MOSH SSH_CONNECTION 10.0.0.2 51000 203.0.113.7 22", &r ).empty() );
  assert( r.sship == "203.0.113.7" );
  ServerReply bad;                                    /* 5 tokens -> fatal */
  assert( !parse_server_line( "MOSH SSH_CONNECTION 10.0.0.2 51000 203.0.113.7", &bad ).empty() );
}

static void test_parse_mosh_ip_banner_and_redefine()
{
  ServerReply r;
  assert( parse_server_line( "MOSH IP 203.0.113.9", &r ).empty() );
  assert( r.mosh_ip == "203.0.113.9" );
  assert( parse_server_line( "Last login: Tue ...", &r ).empty() );  /* banner ignored */
  assert( r.mosh_ip == "203.0.113.9" && !r.have_connect );
  assert( !parse_server_line( "MOSH IP 198.51.100.1", &r ).empty() );  /* redefine -> fatal */
}

static void test_is_numeric_ip()
{
  mosh_winsock_init();
  assert( is_numeric_ip( "203.0.113.7" ) && is_numeric_ip( "::1" ) && is_numeric_ip( "2001:db8::1" ) );
  assert( !is_numeric_ip( "example.com" ) && !is_numeric_ip( "" ) );
  assert( !is_numeric_ip( std::string( "203.0.113.7\0junk", 15 ) ) );
}

static void test_resolve_endpoint()
{
  mosh_winsock_init();
  BootstrapResult out;
  ServerReply a; a.sship = "203.0.113.7"; a.port = "60001"; a.key = "ABCDEFGHIJKLMNOPQRSTUV"; a.have_connect = true;
  assert( resolve_endpoint( a, "host", &out ).empty() && out.ip == "203.0.113.7" && out.port == "60001" );
  ServerReply b = a; b.mosh_ip = "198.51.100.5";
  assert( resolve_endpoint( b, "host", &out ).empty() && out.ip == "198.51.100.5" );  /* MOSH IP wins */
  ServerReply c; c.port = "60001"; c.key = "ABCDEFGHIJKLMNOPQRSTUV"; c.have_connect = true;  /* no IP */
  assert( !resolve_endpoint( c, "host", &out ).empty() );
  ServerReply d; d.sship = "203.0.113.7";                                              /* no key/port */
  assert( !resolve_endpoint( d, "host", &out ).empty() );
  ServerReply e = a; e.mosh_ip = "not-an-ip";                                          /* non-numeric */
  assert( !resolve_endpoint( e, "host", &out ).empty() );
  ServerReply f = a; f.port = "70000";                                                 /* port range */
  assert( !resolve_endpoint( f, "host", &out ).empty() );
}

static void make_pipe( HANDLE *rd, HANDLE *wr )
{
  SECURITY_ATTRIBUTES sa = {}; sa.nLength = sizeof sa; sa.bInheritHandle = FALSE;
  assert( CreatePipe( rd, wr, &sa, 0 ) );
}

static void test_drain_over_pipe()
{
  HANDLE rd = NULL, wr = NULL;
  make_pipe( &rd, &wr );
  const char *canned =
    "Last login: Tue ...\r\n"
    "MOSH SSH_CONNECTION 10.0.0.2 51000 203.0.113.7 22\r\n"
    "MOSH CONNECT 60001 ABCDEFGHIJKLMNOPQRSTUV\r\n";
  DWORD wrote = 0; assert( WriteFile( wr, canned, (DWORD) strlen( canned ), &wrote, NULL ) );
  CloseHandle( wr );
  ServerReply r;
  assert( drain_and_parse( rd, &r ).empty() );
  CloseHandle( rd );
  assert( r.have_connect && r.port == "60001" && r.key == "ABCDEFGHIJKLMNOPQRSTUV" && r.sship == "203.0.113.7" );
}

static void test_drain_eof_without_connect()
{
  HANDLE rd = NULL, wr = NULL;
  make_pipe( &rd, &wr );
  const char *canned = "some ssh diagnostic on stdout, no protocol\r\n";  /* comment: real ssh errors go to stderr/console, not this pipe */
  DWORD wrote = 0; WriteFile( wr, canned, (DWORD) strlen( canned ), &wrote, NULL );
  CloseHandle( wr );
  ServerReply r;
  assert( drain_and_parse( rd, &r ).empty() && !r.have_connect );  /* clean EOF, no fatal */
  CloseHandle( rd );
  BootstrapResult out;
  assert( !resolve_endpoint( r, "host", &out ).empty() );
}

static void test_drain_fatal_line()
{
  HANDLE rd = NULL, wr = NULL;
  make_pipe( &rd, &wr );
  const char *canned = "MOSH CONNECT 60001 bad\r\n";  /* malformed -> fatal */
  DWORD wrote = 0; WriteFile( wr, canned, (DWORD) strlen( canned ), &wrote, NULL );
  CloseHandle( wr );
  ServerReply r;
  assert( !drain_and_parse( rd, &r ).empty() );  /* fatal error returned */
  CloseHandle( rd );
}

static void test_build_ssh_command_line()
{
  const std::wstring ssh_path = L"C:\\Windows\\System32\\OpenSSH\\ssh.exe";
  const std::wstring expected =
    L"\"C:\\Windows\\System32\\OpenSSH\\ssh.exe\" -n -S none -o ProxyJump=none -o ProxyCommand=none user@host -- "
    L"\"sh -c '[ -n \\\"$SSH_CONNECTION\\\" ] && printf \\\"\\nMOSH SSH_CONNECTION %s\\n\\\" \\\"$SSH_CONNECTION\\\"' && mosh-server 'new' '-c' '256' '-s' '-l' 'LC_ALL=C.UTF-8'\"";
  assert( build_ssh_command_line( ssh_path, "user@host" ) == expected );
}

static void test_mosh_bootstrap_rejects_invalid_target()
{
  BootstrapResult out;
  assert( !mosh_bootstrap( NULL, &out ).empty() );
  assert( !mosh_bootstrap( "", &out ).empty() );
  assert( !mosh_bootstrap( "-X", &out ).empty() );
  assert( !mosh_bootstrap( "user\x01@host", &out ).empty() );  /* control char */
  assert( !mosh_bootstrap( "user@h\x7fost", &out ).empty() );  /* DEL */
  assert( !mosh_bootstrap( "user@\xC3\xA9host", &out ).empty() );  /* non-ASCII (eacute) */
}

int main()
{
  test_sh_quote();
  test_build_remote_command();
  test_win_quote_arg();
  test_parse_connect();
  test_parse_ssh_connection();
  test_parse_mosh_ip_banner_and_redefine();
  test_is_numeric_ip();
  test_resolve_endpoint();
  test_drain_over_pipe();
  test_drain_eof_without_connect();
  test_drain_fatal_line();
  test_build_ssh_command_line();
  test_mosh_bootstrap_rejects_invalid_target();
  puts( "test_bootstrap: passed" );
  return 0;
}
