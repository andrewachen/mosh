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

/* ABOUTME: WinSock2 includes, mosh_socket_t, and errno-mapping shims for the mosh network port. */
/* ABOUTME: Included only on _WIN32; POSIX builds are unaffected. */
#pragma once

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstddef>
#include <cstdint>
#include <cwchar>

typedef SOCKET mosh_socket_t;               /* pointer-sized; POSIX side uses int */
const char* wsa_strerror( int err );        /* defined in wincompat.cc */
void mosh_winsock_init( void );             /* idempotent WSAStartup */
int wcwidth( wchar_t ch );                  /* implemented through mosh_win32_wcwidth() */

/* Windows wchar_t is a 16-bit UTF-16 code unit, so it cannot carry an
   astral-plane Unicode scalar value. The engine's decoded-character channel
   is char32_t, a full Unicode code point 0..0x10FFFF; the decode and encode
   shims below bridge the UTF-16 mbrtowc / wcrtomb RTL to that channel. */

/* Decode at most n bytes of UTF-8 from s into one Unicode scalar.
   Decodes UTF-8 directly rather than bridging the UTF-16 mbrtowc RTL, whose
   per-call surrogate-pair contract is unusable (a full 4-byte astral sequence
   decodes to U+FFFD). The port forces a UTF-8 locale at startup, so the input
   encoding is guaranteed. Mirrors mbrtowc's contract: returns the bytes
   consumed, (size_t)-2 for an incomplete sequence, (size_t)-1 with
   errno=EILSEQ for an invalid sequence, and 0 when the decoded scalar is NUL.
   Overlong encodings and UTF-8-encoded surrogates are rejected as ill-formed. */
size_t mosh_mbrtoc32( char32_t* pc32, const char* s, size_t n, mbstate_t* ps );

/* Encode one Unicode scalar as UTF-8 into s (which must hold MB_LEN_MAX
   bytes). Returns the bytes written, or (size_t)-1 with errno=EILSEQ for a
   value outside 0..0x10FFFF or in the surrogate range. */
size_t mosh_c32rtomb( char* s, char32_t c32, mbstate_t* ps );

#endif /* _WIN32 */
