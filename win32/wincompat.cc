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

#include <cerrno>
#include <cwchar>
#include <stdexcept>
#include <string>

#include "win32/wincompat.h"
#include "win32/wcwidth.h"

int wcwidth( wchar_t ch )
{
  return mosh_win32_wcwidth( ch );
}

size_t mosh_mbrtoc32( mosh_char_t* pc32, const char* s, size_t n, mbstate_t* ps )
{
  /* UCRT mbrtowc under a .UTF-8 locale has no usable per-call surrogate-pair
     contract: fed a full 4-byte astral sequence it returned U+FFFD, and fed
     incrementally it reported -2 (incomplete) at every stage, so the old
     surrogate-buffering shim was broken. The port forces a UTF-8 locale at
     startup (mosh_core.cc), so decode UTF-8 directly. Decoding is stateless:
     the parser buffers partial sequences and re-feeds them, so there is
     nothing to carry across calls. */
  (void)ps;

  if ( n == 0 ) {
    return (size_t)-2; /* no input; mirror mbrtowc */
  }

  const unsigned char* u = reinterpret_cast<const unsigned char*>( s );
  const unsigned char lead = u[0];

  if ( lead <= 0x7fu ) {
    *pc32 = lead;
    return lead == 0 ? 0 : 1; /* a NUL byte decodes to 0, matching mbrtowc */
  }

  if ( lead >= 0xc2u && lead <= 0xdfu ) {
    if ( n < 2 ) {
      return (size_t)-2; /* valid prefix, need the continuation byte */
    }
    if ( ( u[1] & 0xc0u ) != 0x80u ) {
      errno = EILSEQ;
      return (size_t)-1;
    }
    *pc32 = ( lead & 0x1fu ) << 6 | ( u[1] & 0x3fu );
    return 2;
  }

  if ( lead >= 0xe0u && lead <= 0xefu ) {
    if ( n < 2 ) {
      return (size_t)-2;
    }
    if ( ( u[1] & 0xc0u ) != 0x80u ) {
      errno = EILSEQ;
      return (size_t)-1;
    }
    if ( lead == 0xe0u && u[1] < 0xa0u ) {
      errno = EILSEQ; /* overlong encoding */
      return (size_t)-1;
    }
    if ( lead == 0xedu && u[1] >= 0xa0u ) {
      errno = EILSEQ; /* UTF-8-encoded surrogate (U+D800..U+DFFF) */
      return (size_t)-1;
    }
    if ( n < 3 ) {
      return (size_t)-2;
    }
    if ( ( u[2] & 0xc0u ) != 0x80u ) {
      errno = EILSEQ;
      return (size_t)-1;
    }
    *pc32 = ( lead & 0x0fu ) << 12 | ( u[1] & 0x3fu ) << 6 | ( u[2] & 0x3fu );
    return 3;
  }

  if ( lead >= 0xf0u && lead <= 0xf4u ) {
    if ( n < 2 ) {
      return (size_t)-2;
    }
    if ( ( u[1] & 0xc0u ) != 0x80u ) {
      errno = EILSEQ;
      return (size_t)-1;
    }
    if ( lead == 0xf0u && u[1] < 0x90u ) {
      errno = EILSEQ; /* overlong encoding */
      return (size_t)-1;
    }
    if ( lead == 0xf4u && u[1] > 0x8fu ) {
      errno = EILSEQ; /* encodes a value above U+10FFFF */
      return (size_t)-1;
    }
    if ( n < 3 ) {
      return (size_t)-2;
    }
    if ( ( u[2] & 0xc0u ) != 0x80u ) {
      errno = EILSEQ;
      return (size_t)-1;
    }
    if ( n < 4 ) {
      return (size_t)-2;
    }
    if ( ( u[3] & 0xc0u ) != 0x80u ) {
      errno = EILSEQ;
      return (size_t)-1;
    }
    *pc32 = ( lead & 0x07u ) << 18 | ( u[1] & 0x3fu ) << 12 | ( u[2] & 0x3fu ) << 6 | ( u[3] & 0x3fu );
    return 4;
  }

  /* 0x80..0xC1 (lone continuation byte or overlong lead) and 0xF5..0xFF
     (above U+10FFFF) are never valid first bytes. */
  errno = EILSEQ;
  return (size_t)-1;
}

size_t mosh_c32rtomb( char* s, mosh_char_t c32, mbstate_t* ps )
{
  (void)ps; /* UTF-8 encoding is stateless */
  if ( c32 > 0x10ffffu || ( c32 >= 0xd800u && c32 <= 0xdfffu ) ) {
    errno = EILSEQ;
    return (size_t)-1;
  }
  if ( c32 <= 0x7fu ) {
    s[0] = static_cast<char>( c32 );
    return 1;
  }
  if ( c32 <= 0x7ffu ) {
    s[0] = static_cast<char>( 0xc0u | ( c32 >> 6 ) );
    s[1] = static_cast<char>( 0x80u | ( c32 & 0x3fu ) );
    return 2;
  }
  if ( c32 <= 0xffffu ) {
    s[0] = static_cast<char>( 0xe0u | ( c32 >> 12 ) );
    s[1] = static_cast<char>( 0x80u | ( ( c32 >> 6 ) & 0x3fu ) );
    s[2] = static_cast<char>( 0x80u | ( c32 & 0x3fu ) );
    return 3;
  }
  s[0] = static_cast<char>( 0xf0u | ( c32 >> 18 ) );
  s[1] = static_cast<char>( 0x80u | ( ( c32 >> 12 ) & 0x3fu ) );
  s[2] = static_cast<char>( 0x80u | ( ( c32 >> 6 ) & 0x3fu ) );
  s[3] = static_cast<char>( 0x80u | ( c32 & 0x3fu ) );
  return 4;
}

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
