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

/* ABOUTME: Regression test for Windows terminal-emulator character widths. */
/* ABOUTME: Verifies combining, wide, and unprintable characters preserve framebuffer semantics. */

#include <cassert>
#include <clocale>
#include <cstdio>
#include <string>
#include <vector>

#include "src/terminal/parser.h"
#include "src/terminal/terminal.h"
#include "win32/wcwidth.h"
#include "win32/wincompat.h"

static void assert_overlay_width( wchar_t ch, int expected_width )
{
  assert( wcwidth( ch ) == expected_width );
}

/* Astral scalar values do not fit in a 16-bit Windows wchar_t, so width for
   them is checked through the scalar channel, not the wchar_t wcwidth(). */
static void assert_scalar_width( mosh_char_t scalar, int expected_width )
{
  assert( mosh_win32_wcwidth_scalar( scalar ) == expected_width );
}

static void print( Terminal::Emulator& emulator, wchar_t ch )
{
  Parser::Print action;
  action.char_present = true;
  action.ch = ch;
  action.act_on_terminal( &emulator );
}

/* Feed raw UTF-8 bytes through the real decode layer and parser, applying any
   resulting Print actions to the emulator. */
static void print_utf8( Terminal::Emulator& emulator, const std::string& bytes )
{
  Parser::UTF8Parser parser;
  for ( const char c : bytes ) {
    Parser::Actions actions;
    parser.input( c, actions );
    for ( Parser::Actions::iterator it = actions.begin(); it != actions.end(); it++ ) {
      if ( typeid( **it ) == typeid( Parser::Print ) ) {
        ( *it )->act_on_terminal( &emulator );
      }
    }
  }
}

int main()
{
#ifdef _WIN32
  assert( setlocale( LC_ALL, ".UTF-8" ) != nullptr );
#endif

  assert_overlay_width( L'́', 0 );
  assert_overlay_width( L'一', 2 );
  assert_overlay_width( L'\x001f', -1 );
  /* An isolated surrogate code unit is non-printing, not a narrow cell. */
  assert_overlay_width( static_cast<wchar_t>( 0xd83d ), -1 );

  Terminal::Emulator combining( 8, 1 );
  print( combining, L'e' );
  print( combining, L'́' );
  const Terminal::Framebuffer& combining_fb = combining.get_fb();
  assert( combining_fb.ds.get_cursor_col() == 1 );
  assert( combining_fb.get_cell( 0, 0 )->debug_contents() == "'e\xcc\x81' [0x65, 0xcc, 0x81]" );
  assert( combining_fb.get_cell( 0, 1 )->empty() );

  Terminal::Emulator wide( 8, 1 );
  print( wide, L'一' );
  const Terminal::Framebuffer& wide_fb = wide.get_fb();
  assert( wide_fb.ds.get_cursor_col() == 2 );
  assert( wide_fb.get_cell( 0, 0 )->get_wide() );
  assert( wide_fb.get_cell( 0, 1 )->empty() );

  Terminal::Emulator unprintable( 8, 1 );
  print( unprintable, L'\x001f' );
  const Terminal::Framebuffer& unprintable_fb = unprintable.get_fb();
  assert( unprintable_fb.ds.get_cursor_col() == 0 );
  assert( unprintable_fb.get_cell( 0, 0 )->empty() );

  /* Astral-plane scalars (emoji, CJK Ext B+) are wide, like their BMP
     wide counterparts. U+1F600 is the canonical emoji; U+20000 is CJK Ext B;
     U+1F200 is Enclosed Ideographic Supplement. */
  assert_scalar_width( 0x1f600, 2 );
  assert_scalar_width( 0x20000, 2 );
  assert_scalar_width( 0x1f200, 2 );
  assert_scalar_width( 0x1f004, 2 );
  assert_scalar_width( 0x1f3f4, 2 );
  /* An astral scalar that is not in a wide range is narrow, not dropped.
     U+1F700 is an alchemical symbol, which glibc classifies narrow. */
  assert_scalar_width( 0x10000, 1 );
  assert_scalar_width( 0x1f700, 1 );
  /* Zero-width astral scalars: tags and variation selectors are format
     characters that attach to the preceding cell. */
  assert_scalar_width( 0xe0001, 0 );
  assert_scalar_width( 0xe0020, 0 );
  assert_scalar_width( 0xe007f, 0 );
  assert_scalar_width( 0xe0100, 0 );
  assert_scalar_width( 0x16fe4, 0 );
  assert_scalar_width( 0x10a01, 0 );
  assert_scalar_width( 0x101fd, 0 );
  assert_scalar_width( 0x1bca0, 0 );
  assert_scalar_width( 0x11001, 0 );
  assert_scalar_width( 0x1da00, 0 );
  assert_scalar_width( 0x10ffff, -1 );
  assert_scalar_width( 0x1f266, -1 );
  assert_scalar_width( 0x2a6e0, -1 );

  /* End to end: a 4-byte UTF-8 emoji decodes to one wide cell holding the
     original UTF-8 bytes, and the cursor advances two columns. */
  Terminal::Emulator emoji( 8, 1 );
  print_utf8( emoji, "\xf0\x9f\x98\x80" ); /* U+1F600 GRINNING FACE */
  const Terminal::Framebuffer& emoji_fb = emoji.get_fb();
  assert( emoji_fb.ds.get_cursor_col() == 2 );
  assert( emoji_fb.get_cell( 0, 0 )->get_wide() );
  assert( emoji_fb.get_cell( 0, 0 )->debug_contents() == "'\xf0\x9f\x98\x80' [0xf0, 0x9f, 0x98, 0x80]" );
  assert( emoji_fb.get_cell( 0, 1 )->empty() );

  /* A BMP wide character still decodes to one wide cell through the same path. */
  Terminal::Emulator bmp_wide( 8, 1 );
  print_utf8( bmp_wide, "\xe4\xb8\x80" ); /* U+4E00 CJK 一 */
  const Terminal::Framebuffer& bmp_wide_fb = bmp_wide.get_fb();
  assert( bmp_wide_fb.ds.get_cursor_col() == 2 );
  assert( bmp_wide_fb.get_cell( 0, 0 )->get_wide() );
  assert( bmp_wide_fb.get_cell( 0, 0 )->debug_contents() == "'\xe4\xb8\x80' [0xe4, 0xb8, 0x80]" );

  puts( "test_terminal_width: passed" );
  return 0;
}
