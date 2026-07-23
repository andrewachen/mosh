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

/* ABOUTME: Idempotent WSAStartup and wsa_strerror() for the mosh network port. */
/* ABOUTME: Called by every entry point that opens a socket; Socket/Connection do NOT self-init. */
#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdexcept>
#include <string>

#include "win32/wincompat.h"

/* WSAStartup is idempotent - calling multiple times is safe */
void mosh_winsock_init( void )
{
  static int initialized = 0;
  if ( !initialized ) {
    WSADATA wsa_data;
    int wserr = WSAStartup( MAKEWORD( 2, 2 ), &wsa_data );
    if ( wserr != 0 ) {
      throw std::runtime_error( std::string( "WSAStartup failed: " ) + wsa_strerror( wserr ) );
    }
    initialized = 1;
  }
}

/* Map WinSock error codes to human-readable strings */
const char* wsa_strerror( int err )
{
  static char buf[1024];
  DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;

  LPVOID msg_buf = NULL;
  if ( FormatMessageA( flags, NULL, err, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),
                       (LPSTR)&msg_buf, 0, NULL ) ) {
    /* Copy to static buffer, stripping trailing CR/LF */
    strncpy( buf, (char*)msg_buf, sizeof( buf ) - 1 );
    buf[sizeof( buf ) - 1] = '\0';
    size_t len = strlen( buf );
    while ( len > 0 && ( buf[len-1] == '\r' || buf[len-1] == '\n' ) ) {
      buf[--len] = '\0';
    }
    LocalFree( msg_buf );
    return buf;
  }

  /* Fallback if FormatMessage fails */
  const char* fallback = "WinSock error";
  size_t fallback_len = strlen( fallback );
  if ( fallback_len < sizeof( buf ) ) {
    memcpy( buf, fallback, fallback_len + 1 );
  } else {
    memcpy( buf, fallback, sizeof( buf ) - 1 );
    buf[sizeof( buf ) - 1] = '\0';
  }
  return buf;
}

#endif /* _WIN32 */
