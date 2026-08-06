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

/* ABOUTME: Windows implementation of the one-code-unit wcwidth semantics mosh uses. */
/* ABOUTME: Shared by the terminal emulator and Win32 wrapper to keep framebuffer and prediction widths aligned. */
#pragma once

#ifdef _WIN32

/* Self-contained: a guarded no-op under the build's forced config include. */
#include <windows.h>

/* Do NOT declare the extern `int wcwidth( wchar_t )` here: src/terminal/terminal.cc
   defines a file-local `static wcwidth`, and a visible extern declaration would make
   that a clang "static declaration follows non-static declaration" error. The extern
   lives in win32/wincompat.h; see win32/config.h.clangarm64.

   Mosh passes one wchar_t at a time; on Windows wchar_t is a UTF-16 code unit, so
   astral Unicode scalar values cannot reach this function as a single character.
   Treat isolated surrogate code units as non-printing rather than misclassifying
   either half as wide. */
static inline int mosh_win32_wcwidth( wchar_t ch )
{
  const unsigned int c = static_cast<unsigned int>( ch );

  if ( c == 0 ) {
    return 0;
  }
  if ( c < 0x20 || ( c >= 0x7f && c < 0xa0 ) || ( c >= 0xd800 && c <= 0xdfff ) ) {
    return -1;
  }

  WORD type = 0;
  if ( GetStringTypeW( CT_CTYPE3, &ch, 1, &type ) && ( type & C3_NONSPACING ) ) {
    return 0;
  }

  return ( c >= 0x1100 && ( c <= 0x115f || c == 0x2329 || c == 0x232a || ( c >= 0x2e80 && c <= 0xa4cf )
                             || ( c >= 0xac00 && c <= 0xd7a3 ) || ( c >= 0xf900 && c <= 0xfaff )
                             || ( c >= 0xfe10 && c <= 0xfe19 ) || ( c >= 0xfe30 && c <= 0xfe6f )
                             || ( c >= 0xff00 && c <= 0xff60 ) || ( c >= 0xffe0 && c <= 0xffe6 ) ) )
           ? 2
           : 1;
}

#endif /* _WIN32 */
