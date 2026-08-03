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

#include "src/terminal/terminal.h"
#include "win32/wincompat.h"

static void assert_overlay_width( wchar_t ch, int expected_width )
{
  assert( wcwidth( ch ) == expected_width );
}

static void print( Terminal::Emulator& emulator, wchar_t ch )
{
  Parser::Print action;
  action.char_present = true;
  action.ch = ch;
  action.act_on_terminal( &emulator );
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

  puts( "test_terminal_width: passed" );
  return 0;
}
