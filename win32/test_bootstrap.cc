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
#include <thread>
#include <vector>

/* Bounds mirrored from the static consts in mosh_bootstrap.cc (MAX_LINE/MAX_TOTAL),
   which are file-static there and so not exposed for symbolic inclusion here. */
static const size_t TEST_MAX_LINE = 64 * 1024;
static const size_t TEST_MAX_TOTAL = 1024 * 1024;

/* Write |data| to |wr| on a background thread, then close the write end. Used for
   any drain test whose input could fill the ~64 KiB default pipe buffer: drain_and_parse
   reads on the CALLING thread and stops early on a fatal line, so a synchronous writer
   of large input would deadlock once the pipe fills and the reader has bailed. */
static void write_all_and_close( HANDLE wr, const char *data, size_t n )
{
  DWORD off = 0;
  while ( off < n ) {
    DWORD once = 0;
    const DWORD chunk = (DWORD) ( ( n - off > 4096 ) ? 4096 : ( n - off ) );
    if ( !WriteFile( wr, data + off, chunk, &once, NULL ) ) break;
    off += once;
  }
  CloseHandle( wr );
}

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

/* Boundary: a single line of exactly MAX_LINE bytes (banner, not MOSH-) followed
   by CRLF is accepted (not over-long) and yields no CONNECT. Written from a
   thread because 64 KiB+ can fill the pipe buffer. */
static void test_drain_line_at_max()
{
  HANDLE rd = NULL, wr = NULL;
  make_pipe( &rd, &wr );
  std::string line( TEST_MAX_LINE, 'x' );          /* exactly MAX_LINE non-protocol chars */
  line += "\r\n";
  std::thread( write_all_and_close, wr, line.data(), line.size() ).detach();
  ServerReply r;
  assert( drain_and_parse( rd, &r ).empty() );     /* ok, no fatal */
  CloseHandle( rd );
  assert( !r.have_connect );
}

/* Boundary: a line that exceeds MAX_LINE with no newline until past the limit is
   fatal BEFORE parse/echo. drain_and_parse returns non-empty. Writer thread. */
static void test_drain_line_over_max()
{
  HANDLE rd = NULL, wr = NULL;
  make_pipe( &rd, &wr );
  std::string line( TEST_MAX_LINE + 1, 'x' );      /* one byte over the limit, no newline */
  std::thread( write_all_and_close, wr, line.data(), line.size() ).detach();
  ServerReply r;
  assert( !drain_and_parse( rd, &r ).empty() );    /* fatal: over-long line */
  CloseHandle( rd );
  assert( !r.have_connect );
}

/* A MOSH CONNECT line carrying an embedded NUL must NOT be accepted as a clean
   protocol message. drain_and_parse treats embedded NULs as fatal (non-empty
   return) before parse_server_line runs, so have_connect stays false. */
static void test_drain_embedded_nul_line()
{
  HANDLE rd = NULL, wr = NULL;
  make_pipe( &rd, &wr );
  /* Valid 22-char key, but a NUL injected before the trailing newline. Build the
     buffer explicitly so the NUL and its size are unambiguous (a string literal
     with \0 would truncate at strlen). */
  std::string canned = "MOSH CONNECT 60001 ABCDEFGHIJKLMNOPQRSTUV";
  canned.push_back( '\0' );
  canned += "\r\n";
  std::thread( write_all_and_close, wr, canned.data(), canned.size() ).detach();
  ServerReply r;
  assert( !drain_and_parse( rd, &r ).empty() );    /* fatal: embedded NUL */
  CloseHandle( rd );
  assert( !r.have_connect );
}

/* Overflow: many banner lines totalling more than MAX_TOTAL is fatal. 1 MiB input
   from a writer thread (a synchronous writer would deadlock against the early
   bail). Lines are MOSH-prefixed but not a recognized sub-command, so parse_server_line
   ignores them as banners (no fprintf, no redefine-fatal) and the total accumulator
   is what trips. */
static void test_drain_total_overflow()
{
  HANDLE rd = NULL, wr = NULL;
  make_pipe( &rd, &wr );
  const char banner[] = "MOSH BANNER ok\r\n";
  const size_t blen = sizeof( banner ) - 1;
  const size_t want = TEST_MAX_TOTAL + 64;         /* strictly over the cap */
  std::vector<char> buf;
  buf.reserve( want );
  while ( buf.size() + blen <= want ) { buf.insert( buf.end(), banner, banner + blen ); }
  std::thread( write_all_and_close, wr, buf.data(), buf.size() ).detach();
  ServerReply r;
  assert( !drain_and_parse( rd, &r ).empty() );    /* fatal: total overflow */
  CloseHandle( rd );
  assert( !r.have_connect );
}

/* resolve_on_path with an explicit fixture directory (NOT the process PATH):
   a present file resolves to that path; a missing file returns a non-empty
   error. Cleans up the temp dir/file at the end. */
static void test_resolve_on_path_fixture()
{
  wchar_t tmp[MAX_PATH] = { 0 };
  assert( GetTempPathW( MAX_PATH, tmp ) );
  wchar_t dir[MAX_PATH] = { 0 };
  assert( GetTempFileNameW( tmp, L"moshbt", 0, dir ) );  /* reserves a temp name */
  /* GetTempFileName gives a file; delete it and make a directory of the same name. */
  DeleteFileW( dir );
  assert( CreateDirectoryW( dir, NULL ) );
  const std::wstring fixture = dir;

  const wchar_t *sentinel = L"sentinel.exe";
  const std::wstring sfile = fixture + L"\\" + sentinel;
  HANDLE h = CreateFileW( sfile.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL );
  assert( h && h != INVALID_HANDLE_VALUE );
  CloseHandle( h );

  std::wstring out;
  std::string ok = resolve_on_path( fixture, sentinel, &out );
  assert( ok.empty() && out == sfile );

  std::wstring miss;
  std::string bad = resolve_on_path( fixture, L"does_not_exist.exe", &miss );
  assert( !bad.empty() );

  /* cleanup */
  DeleteFileW( sfile.c_str() );
  RemoveDirectoryW( fixture.c_str() );
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
  test_drain_line_at_max();
  test_drain_line_over_max();
  test_drain_embedded_nul_line();
  test_drain_total_overflow();
  test_resolve_on_path_fixture();
  test_build_ssh_command_line();
  test_mosh_bootstrap_rejects_invalid_target();
  puts( "test_bootstrap: passed" );
  return 0;
}
