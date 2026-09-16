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

/* ABOUTME: Pins the Windows wcwidth table to the glibc profile it vendored. */
/* ABOUTME: Covers both BMP regression classes, profile discriminators, and lookup consistency. */

#include <cassert>
#include <cstddef> /* size_t */
#include <cstdint>
#include <cstdio>

#include "win32/wcwidth.h"

/* Every assertion in this file is load-bearing. A future configuration that
   defined NDEBUG would compile them all away and turn this into a test that
   always passes, so refuse to build in that case rather than shipping a
   vacuous green. */
#ifdef NDEBUG
#error "test_wcwidth_profile must be built with assertions enabled (NDEBUG must not be defined)"
#endif

/* Every expected value below was checked against real glibc 2.39 C.UTF-8
   wcwidth() before being written down. */

/* Class 1: the hand-written list called these narrow; glibc calls them wide. */
static void test_narrow_regressions()
{
  const char32_t wide[] = {
    0x231a, 0x231b, 0x23e9, 0x23f0, 0x23f3, 0x25fd, 0x25fe, 0x2614, 0x2615,
    0x2648, 0x2653, 0x267f, 0x2693, 0x26a1, 0x26aa, 0x26bd, 0x26c4, 0x26ce,
    0x26d4, 0x26ea, 0x26f2, 0x26f5, 0x26fa, 0x26fd,
    0x2705, /* WHITE HEAVY CHECK MARK - the reported screen corruption */
    0x270a, 0x2728,
    0x274c, /* CROSS MARK */
    0x274e, 0x2753, 0x2757, 0x2795, 0x27b0, 0x27bf,
    0x2b1b, 0x2b1c, 0x2b50, 0x2b55,
    0xa960, 0xa97c, /* Hangul Jamo Extended-A */
  };
  for ( const char32_t cp : wide ) {
    const int w = mosh_win32_wcwidth_scalar( cp );
    if ( w != 2 ) {
      std::fprintf( stderr, "FAIL U+%04X: expected width 2, got %d\n", (unsigned)cp, w );
      assert( false );
    }
  }
  /* Non-emoji-presentation marks in the same blocks stay narrow: proves the
     table did not simply widen a whole range. */
  assert( mosh_win32_wcwidth_scalar( 0x2713 ) == 1 ); /* CHECK MARK */
  assert( mosh_win32_wcwidth_scalar( 0x2717 ) == 1 ); /* BALLOT X */
  assert( mosh_win32_wcwidth_scalar( 0x26a0 ) == 1 ); /* WARNING SIGN */
}

/* Class 2: the hand-written list called these wide; glibc does not. */
static void test_over_wide_regressions()
{
  const char32_t not_wide[] = {
    0x2e9a, 0x2ef4, 0x2eff, 0x2fd6, 0x2fef,
    0x303f, /* assigned, width 1 despite sitting beside unassigned 0x3040 */
    0x3040, 0x3097, 0x309a, 0x3100, 0x3104, 0x3130, 0x318f,
    0x31e4, 0x31ee, 0x321f, 0xa48d, 0xa4c7, 0xa4cf, 0xfa6e, 0xfada, 0xfaff,
    0xfe53, 0xfe67, 0xfe6c, 0xff00,
  };
  for ( const char32_t cp : not_wide ) {
    const int w = mosh_win32_wcwidth_scalar( cp );
    if ( w == 2 ) {
      std::fprintf( stderr, "FAIL U+%04X: expected not-wide, got 2\n", (unsigned)cp );
      assert( false );
    }
  }
  assert( mosh_win32_wcwidth_scalar( 0x303f ) == 1 );
  assert( mosh_win32_wcwidth_scalar( 0x3040 ) == -1 ); /* unassigned */
  /* Two of the old over-wide scalars are combining marks, so their correct
     width is 0, not 1. */
  assert( mosh_win32_wcwidth_scalar( 0x302a ) == 0 );
  assert( mosh_win32_wcwidth_scalar( 0x302d ) == 0 );
}

/* Scalars glibc's width POLICY moved between 2.39 and 2.40, at the same
   Unicode version. These pin the profile to 2.39, and are why the vendored
   file is keyed by glibc release rather than by Unicode version. */
static void test_profile_discriminators()
{
  assert( mosh_win32_wcwidth_scalar( 0x3164 ) == 2 );  /* HANGUL FILLER; 2.40 says 0 */
  assert( mosh_win32_wcwidth_scalar( 0xffa0 ) == 1 );  /* HALFWIDTH HANGUL FILLER; 2.40 says 0 */
  assert( mosh_win32_wcwidth_scalar( 0xfff9 ) == 0 );  /* INTERLINEAR ANNOTATION ANCHOR; 2.40 says 1 */
  assert( mosh_win32_wcwidth_scalar( 0x13430 ) == 0 ); /* EGYPTIAN HIEROGLYPH VERTICAL JOINER; 2.40 says 1 */
}

/* Printability, including the cases a filtered derivation most easily gets
   wrong. U+00AD and U+0600 are NOT in the WIDTH section: they are 1 because
   they are absent from it and fall through to the default. */
static void test_printability()
{
  assert( mosh_win32_wcwidth_scalar( 0x0000 ) == 0 );  /* NUL: width 0, not -1 */
  assert( mosh_win32_wcwidth_scalar( 0x001f ) == -1 ); /* C0 control */
  assert( mosh_win32_wcwidth_scalar( 0x007f ) == -1 ); /* DEL */
  assert( mosh_win32_wcwidth_scalar( 0x0301 ) == 0 );  /* COMBINING ACUTE ACCENT */
  assert( mosh_win32_wcwidth_scalar( 0x200b ) == 0 );  /* ZERO WIDTH SPACE */
  assert( mosh_win32_wcwidth_scalar( 0x00ad ) == 1 );  /* SOFT HYPHEN */
  assert( mosh_win32_wcwidth_scalar( 0x0600 ) == 1 );  /* ARABIC NUMBER SIGN */
  assert( mosh_win32_wcwidth_scalar( 0x2028 ) == -1 ); /* LINE SEPARATOR */
  assert( mosh_win32_wcwidth_scalar( 0x2029 ) == -1 ); /* PARAGRAPH SEPARATOR */
  assert( mosh_win32_wcwidth_scalar( 0xe000 ) == 1 );  /* private use stays printable */
  assert( mosh_win32_wcwidth_scalar( 0xfdd0 ) == -1 ); /* noncharacter */
  assert( mosh_win32_wcwidth_scalar( 0xffff ) == -1 ); /* plane-end noncharacter */
  assert( mosh_win32_wcwidth_scalar( 0xd800 ) == -1 ); /* surrogate */
  assert( mosh_win32_wcwidth_scalar( 0x1ffff ) == -1 ); /* unassigned */
  assert( mosh_win32_wcwidth_scalar( 0x2fffd ) == -1 ); /* unassigned CJK Ext B tail */
  assert( mosh_win32_wcwidth_scalar( 0x10ffff ) == -1 ); /* last scalar */
  assert( mosh_win32_wcwidth_scalar( 0x110000 ) == -1 ); /* beyond range */
}

/* Behaviour that must not regress from the previous implementation. */
static void test_unchanged_behavior()
{
  assert( mosh_win32_wcwidth_scalar( 0x0041 ) == 1 );   /* 'A' */
  assert( mosh_win32_wcwidth_scalar( 0x00e9 ) == 1 );   /* e acute */
  assert( mosh_win32_wcwidth_scalar( 0x4e00 ) == 2 );   /* CJK Unified */
  assert( mosh_win32_wcwidth_scalar( 0x1f600 ) == 2 );  /* astral emoji */
  assert( mosh_win32_wcwidth_scalar( 0x20000 ) == 2 );  /* CJK Ext B */
  assert( mosh_win32_wcwidth_scalar( 0x1f700 ) == 1 );  /* alchemical symbol: narrow */
}

/* The wchar_t entry point must agree with the scalar path for BMP values,
   since wincompat.cc's extern wcwidth() routes through it. */
static void test_wchar_forwarder()
{
  assert( mosh_win32_wcwidth( static_cast<wchar_t>( 0x2705 ) ) == 2 );
  assert( mosh_win32_wcwidth( static_cast<wchar_t>( 0x4e00 ) ) == 2 );
  assert( mosh_win32_wcwidth( static_cast<wchar_t>( 0x0041 ) ) == 1 );
  assert( mosh_win32_wcwidth( static_cast<wchar_t>( 0x001f ) ) == -1 );
  assert( mosh_win32_wcwidth( static_cast<wchar_t>( 0xd83d ) ) == -1 ); /* lone high surrogate */
}

/* Structural invariants the binary search depends on. A conversion error that
   broke these would otherwise be silent. */
static void test_vendored_data_shape()
{
  const size_t wn = sizeof( wcwidth_glibc_2_39_widths ) / sizeof( WcwidthProfileRange );
  const size_t un = sizeof( wcwidth_glibc_2_39_unprintable ) / sizeof( WcwidthProfileRange );
  assert( wn == 482 );
  assert( un == 710 );

  for ( size_t i = 0; i < wn; i++ ) {
    const WcwidthProfileRange& r = wcwidth_glibc_2_39_widths[i];
    assert( r.first <= r.last );
    assert( r.last <= 0x10ffff );
    assert( r.width == 0 || r.width == 2 ); /* overrides are never 1 or -1 */
    if ( i > 0 ) {
      assert( wcwidth_glibc_2_39_widths[i - 1].last < r.first );
    }
  }
  for ( size_t i = 0; i < un; i++ ) {
    const WcwidthProfileRange& r = wcwidth_glibc_2_39_unprintable[i];
    assert( r.first <= r.last );
    assert( r.width == -1 );
    if ( i > 0 ) {
      assert( wcwidth_glibc_2_39_unprintable[i - 1].last < r.first );
    }
  }
}

/* The two arrays must be disjoint. They are (verified for glibc 2.39, 2.40,
   and 2.41 -- localedef applies WIDTH only to printable characters), but the
   runtime's precedence depends on it. */
static void test_arrays_are_disjoint()
{
  const size_t wn = sizeof( wcwidth_glibc_2_39_widths ) / sizeof( WcwidthProfileRange );
  const size_t un = sizeof( wcwidth_glibc_2_39_unprintable ) / sizeof( WcwidthProfileRange );
  for ( size_t i = 0; i < wn; i++ ) {
    for ( size_t j = 0; j < un; j++ ) {
      assert( wcwidth_glibc_2_39_widths[i].last < wcwidth_glibc_2_39_unprintable[j].first
              || wcwidth_glibc_2_39_unprintable[j].last < wcwidth_glibc_2_39_widths[i].first );
    }
  }
}

/* Exhaustive: the binary search must agree with a linear scan of the same
   arrays for every scalar. Catches a faulty lookup, which no sample-based test
   can. It does NOT validate the data -- test_wcwidth_oracle.cc does that
   against a real glibc. */
static int linear_width( uint32_t cp )
{
  if ( cp == 0 ) {
    return 0;
  }
  const size_t wn = sizeof( wcwidth_glibc_2_39_widths ) / sizeof( WcwidthProfileRange );
  const size_t un = sizeof( wcwidth_glibc_2_39_unprintable ) / sizeof( WcwidthProfileRange );
  for ( size_t i = 0; i < un; i++ ) {
    if ( cp >= wcwidth_glibc_2_39_unprintable[i].first
         && cp <= wcwidth_glibc_2_39_unprintable[i].last ) {
      return -1;
    }
  }
  for ( size_t i = 0; i < wn; i++ ) {
    if ( cp >= wcwidth_glibc_2_39_widths[i].first && cp <= wcwidth_glibc_2_39_widths[i].last ) {
      return wcwidth_glibc_2_39_widths[i].width;
    }
  }
  return 1;
}

static void test_lookup_is_exhaustively_consistent()
{
  for ( uint32_t cp = 0; cp <= 0x10ffff; cp++ ) {
    const int got = mosh_win32_wcwidth_scalar( static_cast<char32_t>( cp ) );
    const int want = linear_width( cp );
    if ( got != want ) {
      std::fprintf( stderr, "FAIL U+%04X: binary search %d, linear scan %d\n",
                    (unsigned)cp, got, want );
      assert( false );
    }
  }
}

int main()
{
  test_narrow_regressions();
  test_over_wide_regressions();
  test_profile_discriminators();
  test_printability();
  test_unchanged_behavior();
  test_wchar_forwarder();
  test_vendored_data_shape();
  test_arrays_are_disjoint();
  test_lookup_is_exhaustively_consistent();
  std::printf( "test_wcwidth_profile: all cases passed\n" );
  return 0;
}
