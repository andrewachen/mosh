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
#include "win32/mosh_bootstrap_internal.h"

#include <string>
#include <regex>
#include <sstream>
#include <vector>
#include <new>
#include <process.h>
#include <cstdlib>
#include <cstdio>
#include <getopt.h>
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
  /* mosh.pl:377-389 minimal server args; no trailing -- (no remote command).
     LIMITATION (M3, Linux-first): a Windows client has no Unix locale to forward,
     so the server locale is forced to C.UTF-8. That exists on glibc (the primary
     target) but not on macOS/some BSD, where mosh-server would start non-UTF-8 and
     exit before MOSH CONNECT. Probing the remote for a supported UTF-8 locale is a
     deferred follow-up. */
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
  /* Client UDP target: prefer the server-reported MOSH IP, else the SSH_CONNECTION
     server address (mosh.pl:445-448). Neither of upstream's discovery methods is
     implemented — not the default "proxy" method (a client-side ssh ProxyCommand
     that captures the client-visible address) nor "local" (client-side DNS of the
     target) — so MOSH IP is never populated and the SSH_CONNECTION address is what
     is used. The supported topology is therefore one where that address is the one
     the client can reach: behind NAT or a load balancer it is the server's own,
     often private, address and the UDP session times out. Out of scope by
     decision, not pending; see BUILD-CLANGARM64.md "Known limitations". */
  const std::string ip = !r.mosh_ip.empty() ? r.mosh_ip : r.sship;
  if ( ip.empty() )
    return "mosh: requires a direct UDP endpoint to " + target + "; proxied SSH is unsupported";
  if ( !is_numeric_ip( ip ) )
    return "mosh: server reported a non-numeric address (" + ip + ")";
  out->ip = ip; out->port = r.port; out->key = r.key;
  return "";
}

std::wstring widen( const std::string &s )
{
  if ( s.empty() ) return std::wstring();
  const int need = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int) s.size(), NULL, 0 );
  if ( need <= 0 ) return std::wstring();   // caller treats empty result on non-empty input as failure
  std::wstring w( need, L'\0' );
  MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int) s.size(), &w[0], need );
  return w;
}

std::wstring build_ssh_command_line( const std::wstring &ssh_path, const std::string &target )
{
  /* -T forces no remote PTY; -n takes stdin from NUL. Both are needed and they do
     different things: -n only redirects this side's stdin, while a user's
     RequestTTY=force would still allocate a terminal without -T. Intentional
     divergence from upstream mosh.pl's default -tt. The reason is determinism, not
     a specific decoding failure: we parse mosh-server's startup banner from ssh's
     piped stdout, and a PTY makes that stream configuration-dependent — line
     discipline applies, and stderr arrives merged rather than separate. Pinning it
     keeps one stream shape to parse. The tradeoff is that mosh-server 1.2.4 and
     older, which exit when their initial TIOCGWINSZ fails, are unsupported; 1.2.5
     falls back to 80x24. Whether -T or a forced -tt is the better pin is open;
     see PARITY.md I8. ProxyJump and ProxyCommand are pinned for a reason that
     was never recorded, and they refuse routes that would work: PARITY.md I10
     holds that open question. -S none is NOT part of it and must not be lifted
     with them — connection sharing would hand the plaintext MOSH CONNECT line,
     key included, to a pre-existing control master outside this process's Job
     Object. See PARITY.md S2. */
  return L"\"" + ssh_path + L"\""
    + L" -n -T -S none -o ProxyJump=none -o ProxyCommand=none "
    + widen( win_quote_arg( target ) ) + L" -- "
    + widen( win_quote_arg( build_remote_command() ) );
}

static const size_t MAX_LINE = 64 * 1024;
static const size_t MAX_TOTAL = 1024 * 1024;

std::string drain_and_parse( HANDLE h, ServerReply *r )
{
  std::string pending; size_t total = 0; char buf[4096];
  for ( ;; ) {
    DWORD got = 0;
    if ( !ReadFile( h, buf, sizeof buf, &got, NULL ) ) {
      const DWORD e = GetLastError();
      if ( e == ERROR_BROKEN_PIPE ) break;
      return "mosh: failed reading ssh output (error " + std::to_string( e ) + ")";
    }
    if ( got == 0 ) break;
    total += got;
    if ( total > MAX_TOTAL ) return "mosh: ssh produced too much output before MOSH CONNECT";
    pending.append( buf, got );
    std::string::size_type nl;
    while ( ( nl = pending.find( '\n' ) ) != std::string::npos ) {
      std::string line = pending.substr( 0, nl );
      pending.erase( 0, nl + 1 );
      if ( !line.empty() && line.back() == '\r' ) line.pop_back();
      if ( line.size() > MAX_LINE ) return "mosh: ssh produced an over-long line before MOSH CONNECT";
      if ( line.find( '\0' ) != std::string::npos ) return "mosh: ssh output contained an embedded NUL before MOSH CONNECT";
      const std::string err = parse_server_line( line, r );
      if ( !err.empty() ) return err;
      if ( r->have_connect ) return "";
      if ( line.compare( 0, 5, "MOSH " ) != 0 ) std::fprintf( stdout, "%s\n", line.c_str() );
    }
    if ( pending.size() > MAX_LINE ) return "mosh: ssh produced an over-long line before MOSH CONNECT";
  }
  if ( !pending.empty() ) {
    if ( pending.back() == '\r' ) pending.pop_back();
    if ( pending.size() > MAX_LINE ) return "mosh: ssh produced an over-long line before MOSH CONNECT";
    if ( pending.find( '\0' ) != std::string::npos ) return "mosh: ssh output contained an embedded NUL before MOSH CONNECT";
    const std::string err = parse_server_line( pending, r );
    if ( !err.empty() ) return err;
  }
  return "";
}

std::string resolve_on_path( const std::wstring &path_dirs, const std::wstring &name, std::wstring *out )
{
  // Size query, then resolve. lpPath is explicit (excludes implicit CWD/app-dir search).
  const DWORD need = SearchPathW( path_dirs.c_str(), name.c_str(), L".exe", 0, NULL, NULL );
  if ( need == 0 ) return "mosh: could not find " + std::string( name.begin(), name.end() ) + " on PATH";
  std::wstring buf( need, L'\0' );
  const DWORD n = SearchPathW( path_dirs.c_str(), name.c_str(), L".exe", need, &buf[0], NULL );
  /* The size-query call returned the required size INCLUDING the null (need = L+1).
     On a successful copy SearchPathW returns the length EXCLUDING the null (n = need-1),
     or 0 on failure; the n > need branch is an unreachable defensive truncation guard. */
  if ( n == 0 || n > need ) return "mosh: could not resolve ssh path";
  buf.resize( n );
  *out = buf;
  return "";
}

std::string resolve_ssh_path( std::wstring *out )
{
  const DWORD plen = GetEnvironmentVariableW( L"PATH", NULL, 0 );
  if ( plen == 0 ) return "mosh: PATH is not set; cannot locate ssh.exe";   // fail closed, never NULL lpPath
  std::wstring path_env( plen, L'\0' );
  const DWORD got = GetEnvironmentVariableW( L"PATH", &path_env[0], plen );
  path_env.resize( got );
  return resolve_on_path( path_env, L"ssh", out );
}

namespace {
class JoiningThread {
  HANDLE handle_;

public:
  typedef unsigned (__stdcall *Entry)( void * );

  JoiningThread( Entry entry, void *arg ) : handle_( (HANDLE) _beginthreadex( NULL, 0, entry, arg, 0, NULL ) ) {}
  ~JoiningThread() { if ( handle_ ) { WaitForSingleObject( handle_, INFINITE ); CloseHandle( handle_ ); } }

  HANDLE handle() const { return handle_; }
  void join() { WaitForSingleObject( handle_, INFINITE ); CloseHandle( handle_ ); handle_ = NULL; }
};

struct DrainState {
  HANDLE read_end;
  HANDLE done;
  ServerReply *reply;
  std::string error;
};

unsigned __stdcall drain_thread( void *arg )
{
  DrainState *state = static_cast<DrainState *>( arg );
  try {
    state->error = drain_and_parse( state->read_end, state->reply );
  } catch ( const std::bad_alloc & ) {
    state->error = "mosh: out of memory while reading ssh output";
  } catch ( ... ) {
    state->error = "mosh: failed while reading ssh output";
  }
  SetEvent( state->done );
  return 0;
}

/* Child-inheritable duplicate of a std handle. A VALID handle that fails to
   duplicate is FATAL (never silently substitute NUL for a real console handle
   — that would hide auth prompts). Only an absent/redirected handle falls back
   to NUL. */
std::string dup_or_nul( DWORD which, HANDLE *out )
{
  HANDLE src = GetStdHandle( which );
  if ( src != NULL && src != INVALID_HANDLE_VALUE ) {
    if ( DuplicateHandle( GetCurrentProcess(), src, GetCurrentProcess(), out, 0, TRUE, DUPLICATE_SAME_ACCESS ) )
      return "";
    return "mosh: DuplicateHandle failed for a standard handle";
  }
  SECURITY_ATTRIBUTES sa = {}; sa.nLength = sizeof sa; sa.bInheritHandle = TRUE;
  *out = CreateFileW( L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &sa, OPEN_EXISTING, 0, NULL );
  if ( *out == INVALID_HANDLE_VALUE ) return "mosh: could not open NUL for a standard handle";
  return "";
}
}

/* Launch app_path with cmdline: whitelist only the 3 std handles, confine the
   child to a kill-on-close Job Object ATOMICALLY at creation (no suspend/assign
   window), and drain stdout until a reply, child exit, or lifecycle cancellation.
   Every setup call is checked and fails closed (no CreateProcessW with an
   incomplete whitelist). Reaps the child. */
std::string spawn_and_drain( const std::wstring &app_path, const std::wstring &cmdline,
                             ServerReply *r, HANDLE cancel )
{
  SECURITY_ATTRIBUTES sa = {}; sa.nLength = sizeof sa; sa.bInheritHandle = TRUE;
  HANDLE rd = NULL, wr = NULL;
  if ( !CreatePipe( &rd, &wr, &sa, 0 ) )
    return "mosh: CreatePipe failed (error " + std::to_string( GetLastError() ) + ")";
  if ( !SetHandleInformation( rd, HANDLE_FLAG_INHERIT, 0 ) ) {
    CloseHandle( rd ); CloseHandle( wr ); return "mosh: SetHandleInformation failed";
  }

  HANDLE dupIn = NULL, dupErr = NULL;
  std::string herr = dup_or_nul( STD_INPUT_HANDLE, &dupIn );
  if ( herr.empty() ) herr = dup_or_nul( STD_ERROR_HANDLE, &dupErr );
  if ( !herr.empty() ) {
    if ( dupIn && dupIn != INVALID_HANDLE_VALUE ) CloseHandle( dupIn );
    CloseHandle( rd ); CloseHandle( wr ); return herr;
  }

  HANDLE job = CreateJobObjectW( NULL, NULL );
  bool job_ok = job != NULL;
  if ( job_ok ) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji = {};
    ji.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    job_ok = SetInformationJobObject( job, JobObjectExtendedLimitInformation, &ji, sizeof ji );
  }
  if ( !job_ok ) {
    if ( job ) CloseHandle( job );
    CloseHandle( dupIn ); CloseHandle( dupErr ); CloseHandle( rd ); CloseHandle( wr );
    return "mosh: could not create the ssh containment job";
  }

  HANDLE inherit[3] = { dupIn, wr, dupErr };
  SIZE_T asz = 0;
  InitializeProcThreadAttributeList( NULL, 2, 0, &asz );
  std::vector<char> abuf( asz );
  LPPROC_THREAD_ATTRIBUTE_LIST attr = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>( abuf.data() );
  const bool init_ok = InitializeProcThreadAttributeList( attr, 2, 0, &asz );
  bool attr_ok = init_ok
    && UpdateProcThreadAttribute( attr, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, NULL, NULL )
    && UpdateProcThreadAttribute( attr, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job, sizeof job, NULL, NULL );
  if ( !attr_ok ) {
    /* Only Delete the list if the real Initialize above actually succeeded; the
       size-query call (with a NULL list pointer) never initialized attr. */
    if ( init_ok ) DeleteProcThreadAttributeList( attr );
    CloseHandle( job ); CloseHandle( dupIn ); CloseHandle( dupErr ); CloseHandle( rd ); CloseHandle( wr );
    return "mosh: failed to build the spawn attribute list";   // fail closed, never spawn without the whitelist
  }

  STARTUPINFOEXW six = {}; six.StartupInfo.cb = sizeof six;
  six.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  six.StartupInfo.hStdInput = dupIn; six.StartupInfo.hStdOutput = wr; six.StartupInfo.hStdError = dupErr;
  six.lpAttributeList = attr;

  std::vector<wchar_t> mcmd( cmdline.begin(), cmdline.end() ); mcmd.push_back( L'\0' );
  PROCESS_INFORMATION pi = {};
  const BOOL ok = CreateProcessW( app_path.c_str(), mcmd.data(), NULL, NULL, TRUE,
                                  EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &six.StartupInfo, &pi );
  const DWORD spawn_err = GetLastError();
  DeleteProcThreadAttributeList( attr );
  CloseHandle( wr );
  if ( dupIn && dupIn != INVALID_HANDLE_VALUE ) CloseHandle( dupIn );
  if ( dupErr && dupErr != INVALID_HANDLE_VALUE ) CloseHandle( dupErr );
  if ( !ok ) {
    CloseHandle( rd ); CloseHandle( job );
    return "mosh: could not launch ssh (error " + std::to_string( spawn_err ) + ")";
  }
  CloseHandle( pi.hThread );   // child is in the job atomically at creation; no suspend/assign window

  HANDLE drain_done = CreateEventW( NULL, TRUE, FALSE, NULL );
  if ( !drain_done ) {
    CloseHandle( job ); CloseHandle( pi.hProcess ); CloseHandle( rd );
    return "mosh: could not create the ssh output completion event";
  }
  DrainState state = { rd, drain_done, r, "" };
  JoiningThread drainer( drain_thread, &state );
  if ( !drainer.handle() ) {
    CloseHandle( drain_done ); CloseHandle( job ); CloseHandle( pi.hProcess ); CloseHandle( rd );
    return "mosh: could not start the ssh output reader";
  }

  HANDLE waits[3] = { pi.hProcess, drain_done, cancel };
  const DWORD wait_count = cancel ? 3 : 2;
  const DWORD waited = WaitForMultipleObjects( wait_count, waits, FALSE, INFINITE );
  const bool cancelled = cancel && waited == WAIT_OBJECT_0 + 2;
  if ( cancelled ) TerminateProcess( pi.hProcess, 1 );
  DWORD exit_code = 0;
  const bool have_exit = GetExitCodeProcess( pi.hProcess, &exit_code ) && exit_code != STILL_ACTIVE;
  CloseHandle( job );                                 // kill-on-close reaps ssh and pipe writers
  WaitForSingleObject( pi.hProcess, 2000 );           // confirm reap after job close
  CloseHandle( pi.hProcess );
  std::string cancel_error;
  if ( WaitForSingleObject( drainer.handle(), 2000 ) == WAIT_TIMEOUT ) {
    if ( !CancelSynchronousIo( drainer.handle() ) ) {
      const DWORD cancel_status = GetLastError();
      /* ERROR_NOT_FOUND can mean the read completed between the timeout and
         cancellation. Recheck the native thread handle before reporting it. */
      if ( WaitForSingleObject( drainer.handle(), 2000 ) == WAIT_TIMEOUT )
        cancel_error = "mosh: could not stop reading ssh output (error "
          + std::to_string( cancel_status ) + ")";
    } else {
      WaitForSingleObject( drainer.handle(), 2000 );
    }
  }
  drainer.join();
  CloseHandle( drain_done );
  CloseHandle( rd );

  // Outcome precedence: read/fatal error > cancellation failure > valid CONNECT
  //                     > nonzero ssh exit > missing startup (resolve_endpoint).
  if ( !state.error.empty() ) return state.error;
  if ( !cancel_error.empty() ) return cancel_error;
  if ( cancelled ) return "mosh: bootstrap cancelled";
  if ( r->have_connect ) return "";
  if ( have_exit && exit_code != 0 ) return "mosh: ssh exited with status " + std::to_string( exit_code );
  return "";
}

static bool is_clean_destination( const char *s )
{
  for ( ; *s; ++s ) { const unsigned char c = (unsigned char) *s; if ( c < 0x20 || c >= 0x7f ) return false; }
  return true;
}

bool parse_invocation( int argc, char *argv[], unsigned *verbose, const char **dest )
{
  *verbose = 0;
  optind = 1;   /* restart getopt's scan (mingw is BSD-derived: optind=1, not the glibc 0) */
  opterr = 0;   /* suppress getopt's own error output; we print usage ourselves */
  bool ok = true;
  int opt;
  while ( ( opt = getopt( argc, argv, "v" ) ) != -1 ) {
    if ( opt == 'v' ) {
      ++*verbose;
    } else {
      ok = false;   /* keep scanning to drain getopt's mid-token cursor, so the next call starts clean */
    }
  }
  if ( !ok ) { return false; }
  if ( argc - optind != 1 ) { return false; }
  const char *d = argv[optind];
  if ( d[0] == '\0' || d[0] == '-' ) { return false; }   /* empty or option-looking destination is a usage error */
  *dest = d;
  return true;
}

std::string mosh_bootstrap( const char *target, BootstrapResult *out )
{
  if ( target == NULL || target[0] == '\0' || target[0] == '-' )
    return "mosh: invalid destination";
  if ( !is_clean_destination( target ) ) return "mosh: destination must be printable ASCII (no control characters or non-ASCII)";
  mosh_winsock_init();

  std::wstring ssh_path;
  const std::string rerr = resolve_ssh_path( &ssh_path );
  if ( !rerr.empty() ) return rerr;

  const std::wstring cmdline = build_ssh_command_line( ssh_path, target );
  /* Key hygiene: the base64 session key returned by the server is copied into
     out->key and scrubbed (SecureZeroMemory) by the caller immediately after the
     MoshCore ctor consumes it -- that is the long-lived copy. The short-lived copies
     here (reply.key, and the drain buffers inside spawn_and_drain) are freed when
     this function returns and are left un-zeroed: partial scrubbing of copies that
     std::string may have reallocated cannot be done reliably, and upstream mosh
     zeroes none of its key copies at all. Documented residual for a crash-dump-class
     adversary; the key never touches a command line, environment variable, or log. */
  ServerReply reply;
  const std::string serr = spawn_and_drain( ssh_path, cmdline, &reply );
  if ( !serr.empty() ) return serr;
  return resolve_endpoint( reply, std::string( target ), out );
}
