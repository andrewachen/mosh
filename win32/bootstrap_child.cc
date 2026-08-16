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

/* ABOUTME: Test fixture child for the Win32 spawn-path oracle: emits a fake MOSH CONNECT line. */
/* ABOUTME: Task 4 of M3 (SSH bootstrap) milestone - driven by test_bootstrap fixture tests. */

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cwchar>

/* argv: [1]=sentinel event handle value (decimal), [2]=expected marker, [3]=mode. */
int wmain( int argc, wchar_t **argv )
{
  if ( argc >= 2 ) {
    HANDLE sentinel = (HANDLE)(uintptr_t) _wcstoui64( argv[1], NULL, 10 );
    if ( sentinel && SetEvent( sentinel ) ) { /* inherited: leak */ }
  }
  if ( argc < 3 || wcscmp( argv[2], L"round trip \"marker\"" ) != 0 ) {
    fwprintf( stderr, L"bootstrap_child: argv round-trip failed\n" );
    return 4;
  }
  const bool hang = ( argc >= 4 && wcscmp( argv[3], L"hang" ) == 0 );
  const bool silent = ( argc >= 4 && wcscmp( argv[3], L"silent" ) == 0 );
  const bool delayed = ( argc >= 4 && wcscmp( argv[3], L"delayed" ) == 0 );
  if ( delayed ) Sleep( 11000 );
  if ( !silent ) {
    printf( "MOSH SSH_CONNECTION 10.0.0.2 51000 203.0.113.7 22\n" );
    printf( "MOSH CONNECT 60001 ABCDEFGHIJKLMNOPQRSTUV\n" );
    fflush( stdout );
  }
  if ( hang || silent ) { for ( ;; ) Sleep( 60000 ); }   // stay alive to exercise cleanup
  return 0;
}
