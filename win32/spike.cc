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

// ABOUTME: Build-spike stub proving the mosh engine links into a native ARM64 DLL.
// ABOUTME: Exports one symbol; replaced by the real facade in Milestone 2.

#include "src/crypto/base64.h"
#include "src/protobufs/userinput.pb.h"
#include "src/util/timestamp.h"
#include "src/terminal/terminalframebuffer.h"
#include "src/statesync/completeterminal.h"

extern "C" __declspec( dllexport ) int mosh_spike_ok( void )
{
  const uint8_t raw[] = { 0 };
  char encoded[5];
  base64_encode( raw, sizeof( raw ), encoded, sizeof( encoded ) );
  ClientBuffers::UserMessage message;

  /* util: frozen_timestamp() */
  freeze_timestamp();
  const uint64_t frozen = frozen_timestamp();

  /* terminal: Framebuffer */
  Terminal::Framebuffer fb( 1, 1 );
  const int fb_width = (int)fb.ds.get_width();

  /* statesync: Complete */
  Terminal::Complete comp( 1, 1 );
  (void)comp;

  /* Fold all results into return value to prevent elision */
  return encoded[0] == 'A' && message.ByteSizeLong() == 0 && frozen > 0 && fb_width == 1 ? 42 : 0;
}
