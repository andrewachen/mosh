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

/* ABOUTME: Windows width classification matching a glibc-2.39 C.UTF-8 server. */
/* ABOUTME: Data is vendored from glibc's own locale artifact; see wcwidth_data_glibc_2_39.h. */
#pragma once

#ifdef _WIN32

#include <cstddef> /* size_t */
#include <cstdint>

#include "win32/wcwidth_data_glibc_2_39.h"

/* Find the interval containing scalar in a sorted, disjoint array. Returns
   true and stores its width in *width, or false if no interval contains it.
   Binary search: a linear scan would be O(entries) per character, and CJK
   output hits this on nearly every glyph. */
static inline bool mosh_wcwidth_lookup( const WcwidthProfileRange* ranges,
                                        size_t count,
                                        uint32_t scalar,
                                        int* width )
{
  size_t lo = 0;
  size_t hi = count;
  while ( lo < hi ) {
    const size_t mid = lo + ( hi - lo ) / 2;
    if ( ranges[mid].first <= scalar ) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if ( lo > 0 && scalar <= ranges[lo - 1].last ) {
    *width = ranges[lo - 1].width;
    return true;
  }
  return false;
}

/* Width of a full Unicode scalar value, matching a glibc-2.39 server.

   The server renders with glibc; the client re-emulates the server's output
   and must agree on every character's width or its cursor tracking desyncs.
   The table is vendored from glibc's own locale artifact, so it matches by
   construction rather than by approximation. Where Unicode and glibc
   disagree, glibc wins, because the framebuffer must agree with the server.

   Precedence mirrors glibc's construction: NUL is width 0 before anything
   else, then unprintable scalars are -1, then WIDTH overrides apply, and
   everything else is 1. The two arrays are disjoint, so the order is not
   load-bearing for correctness, but it matches the reference and
   test_wcwidth_profile asserts the disjointness.

   This previously consulted GetStringTypeW for nonspacing marks; the
   vendored table carries that from the server's own locale data, and so
   matches the server where the Windows API need not. */
static inline int mosh_win32_wcwidth_scalar( char32_t scalar )
{
  if ( scalar > 0x10ffff ) {
    return -1;
  }
  const uint32_t cp = static_cast<uint32_t>( scalar );

  if ( cp == 0 ) {
    return 0;
  }

  int width = 0;
  if ( mosh_wcwidth_lookup( wcwidth_glibc_2_39_unprintable,
                            sizeof( wcwidth_glibc_2_39_unprintable ) / sizeof( WcwidthProfileRange ),
                            cp,
                            &width ) ) {
    return width;
  }
  if ( mosh_wcwidth_lookup( wcwidth_glibc_2_39_widths,
                            sizeof( wcwidth_glibc_2_39_widths ) / sizeof( WcwidthProfileRange ),
                            cp,
                            &width ) ) {
    return width;
  }
  return 1;
}

/* Mosh's engine passes one wchar_t at a time; on Windows that is a UTF-16
   code unit, so astral scalars cannot arrive here as a single value. Isolated
   surrogate halves are non-printing rather than misclassified as wide. */
static inline int mosh_win32_wcwidth( wchar_t ch )
{
  return mosh_win32_wcwidth_scalar( static_cast<char32_t>( static_cast<uint16_t>( ch ) ) );
}

#endif /* _WIN32 */
