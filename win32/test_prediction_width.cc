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

/* ABOUTME: Regression test for Windows prediction of Unicode scalar widths. */
/* ABOUTME: Verifies astral scalars take the no-prediction branch while ASCII predicts normally. */

#include <cassert>
#include <cstdio>
#include <string>

#include "src/frontend/terminaloverlay.h"
#include "src/terminal/terminalframebuffer.h"

static void feed_utf8( Overlay::PredictionEngine& predictions,
                       const Terminal::Framebuffer& framebuffer,
                       const std::string& bytes )
{
  for ( const char byte : bytes ) {
    predictions.new_user_byte( byte, framebuffer );
  }
}

int main()
{
  /* Experimental is load-bearing: under the Adaptive default, become_tentative()
     bumps prediction_epoch past confirmed_epoch 0, so ConditionalOverlayCell::apply
     would skip the ASCII control cell and the first assertion would fail. Under
     Experimental, become_tentative() is a no-op and predictions apply directly. */
  Terminal::Framebuffer ascii_framebuffer( 8, 1 );
  Overlay::PredictionEngine ascii_predictions;
  ascii_predictions.set_display_preference( Overlay::PredictionEngine::Experimental );
  feed_utf8( ascii_predictions, ascii_framebuffer, "a" );
  ascii_predictions.apply( ascii_framebuffer );
  assert( ascii_framebuffer.get_cell( 0, 0 )->debug_contents() == "'a' [0x61]" );

  /* An astral scalar (U+1F600) is not width 1, so new_user_byte takes the
     unknown-print branch and installs no prediction cell. Before the scalar
     width fix the engine truncated the char32_t to a 16-bit wchar_t (0xF600,
     PUA, width 1) and predicted a narrow cell here. */
  Terminal::Framebuffer emoji_framebuffer( 8, 1 );
  Overlay::PredictionEngine emoji_predictions;
  emoji_predictions.set_display_preference( Overlay::PredictionEngine::Experimental );
  feed_utf8( emoji_predictions, emoji_framebuffer, "\xf0\x9f\x98\x80" );
  emoji_predictions.apply( emoji_framebuffer );
  assert( emoji_framebuffer.get_cell( 0, 0 )->empty() );

  puts( "test_prediction_width: passed" );
  return 0;
}
