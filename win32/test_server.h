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

/* ABOUTME: Test-only reverse transport server for the MoshCore lifecycle test. */
/* ABOUTME: Applies received UserStream actions to Complete and returns terminal state. */
#ifndef TEST_SERVER_H
#define TEST_SERVER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/* Lives in namespace Terminal because Terminal::Cell befriends it
   (src/terminal/terminalframebuffer.h); a friend declaration's name resolves
   in the innermost enclosing namespace. */
namespace Terminal {

class TestServer {
private:
  class Impl;
  std::unique_ptr<Impl> impl;

public:
  TestServer( int cols, int rows );
  ~TestServer();
  TestServer( const TestServer& ) = delete;
  TestServer& operator=( const TestServer& ) = delete;

  std::string port() const;
  std::string get_key() const;
  std::vector<intptr_t> socket_fds() const;
  void on_readable( intptr_t which_fd );
  void tick();
  /* True when one cell of the server's terminal holds exactly the given UTF-8
     bytes — the echo writeback of whatever the client sent. */
  bool cell_contents_is( int row, int col, const char *bytes, size_t len ) const;
};

} /* namespace Terminal */

#endif
