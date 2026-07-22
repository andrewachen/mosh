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

// ABOUTME: Build-spike stub proving the mosh engine links into a native ARM64 standalone exe.
// ABOUTME: Exercises the AES-OCB crypto core (real libcrypto link) plus one symbol per engine archive.

#include "src/crypto/crypto.h"
#include "src/protobufs/userinput.pb.h"
#include "src/util/timestamp.h"
#include "src/terminal/terminalframebuffer.h"
#include "src/statesync/completeterminal.h"

int main( void )
{
  ClientBuffers::UserMessage message;

  /* crypto: real AES-OCB round-trip. The fixed 128-bit key (all zero, 22 'A's
     in printable form) avoids the PRNG, which cannot draw entropy on Windows
     until the M1 CSPRNG port; the caller-supplied Nonce means encrypt/decrypt
     need no entropy either. This forces ae_init/ae_encrypt/ae_decrypt — the
     OpenSSL-backed cipher path — to link and run, and also exercises base64.cc
     through Base64Key. The crypto core is called, never modified. */
  Crypto::Base64Key key( std::string( 22, 'A' ) );
  Crypto::Session session( key );
  const std::string secret( "mosh" );
  const Crypto::Message plaintext( Crypto::Nonce( uint64_t( 1 ) ), secret );
  const std::string ciphertext = session.encrypt( plaintext );
  const Crypto::Message decrypted = session.decrypt( ciphertext );

  /* util: frozen_timestamp() */
  freeze_timestamp();
  const uint64_t frozen = frozen_timestamp();

  /* terminal: Framebuffer */
  Terminal::Framebuffer fb( 1, 1 );
  const int fb_width = (int)fb.ds.get_width();

  /* statesync: Complete - use out-of-line wait_time() method to prove link */
  Terminal::Complete comp( 1, 1 );
  const int wait = comp.wait_time( frozen );

  /* Fold all results into a clean-exit (0) success code; any mismatch is a
     link or runtime failure. The external archive calls cannot be elided. */
  const bool ok = message.ByteSizeLong() == 0 && decrypted.text == secret
    && frozen > 0 && fb_width == 1 && wait >= 0;
  return ok ? 0 : 1;
}
