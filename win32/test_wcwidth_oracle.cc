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

/* ABOUTME: Validates the vendored width profile against the host's real glibc. */
/* ABOUTME: Linux-only; the CI step selects a runner whose libc matches the profile. */

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <clocale>
#include <cwchar>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

struct Range {
  uint32_t first;
  uint32_t last;
  int width;
};

static std::vector<Range> load_block( const std::string& path,
                                      const std::string& begin,
                                      const std::string& end )
{
  std::ifstream in( path );
  if ( !in ) {
    std::fprintf( stderr, "cannot open %s\n", path.c_str() );
    std::exit( 2 );
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  const size_t b = text.find( begin );
  const size_t e = text.find( end );
  if ( b == std::string::npos || e == std::string::npos || e < b ) {
    std::fprintf( stderr, "missing marker %s / %s in %s\n", begin.c_str(), end.c_str(), path.c_str() );
    std::exit( 2 );
  }
  const std::string block = text.substr( b, e - b );
  const std::regex re( R"(\{ 0x([0-9a-fA-F]+), 0x([0-9a-fA-F]+), (-?[0-9]+) \})" );
  std::vector<Range> out;
  for ( std::sregex_iterator it( block.begin(), block.end(), re ), last; it != last; ++it ) {
    out.push_back( { (uint32_t)std::stoul( ( *it )[1].str(), nullptr, 16 ),
                     (uint32_t)std::stoul( ( *it )[2].str(), nullptr, 16 ),
                     std::stoi( ( *it )[3].str() ) } );
  }
  return out;
}

static int width_of( const std::vector<Range>& unprintable,
                     const std::vector<Range>& widths,
                     uint32_t cp )
{
  if ( cp == 0 ) {
    return 0;
  }
  for ( const Range& r : unprintable ) {
    if ( cp >= r.first && cp <= r.last ) {
      return -1;
    }
  }
  for ( const Range& r : widths ) {
    if ( cp >= r.first && cp <= r.last ) {
      return r.width;
    }
  }
  return 1;
}

int main( int argc, char** argv )
{
  const char* path = argc > 1 ? argv[1] : "win32/wcwidth_data_glibc_2_39.h";

  if ( !std::setlocale( LC_ALL, "C.UTF-8" ) ) {
    std::fprintf( stderr, "FAIL: cannot set C.UTF-8 locale\n" );
    return 1;
  }

  const std::vector<Range> widths
    = load_block( path, "BEGIN VENDORED WIDTH", "END VENDORED WIDTH." );
  const std::vector<Range> unprintable
    = load_block( path, "BEGIN VENDORED UNPRINTABLE", "END VENDORED UNPRINTABLE." );

  if ( widths.size() != 482 || unprintable.size() != 710 ) {
    std::fprintf( stderr, "FAIL: parsed %zu widths and %zu unprintable intervals, expected 482 and 710\n",
                  widths.size(), unprintable.size() );
    return 1;
  }

  long mismatches = 0;
  long shown = 0;
  for ( uint32_t cp = 0; cp <= 0x10ffff; cp++ ) {
    const int want = width_of( unprintable, widths, cp );
    const int got = wcwidth( static_cast<wchar_t>( cp ) );
    if ( want != got ) {
      mismatches++;
      if ( shown < 20 ) {
        std::fprintf( stderr, "  U+%04X vendored=%d real glibc=%d\n", cp, want, got );
        shown++;
      }
    }
  }
  if ( mismatches ) {
    std::fprintf( stderr, "FAIL: %ld mismatches against real glibc\n", mismatches );
    return 1;
  }
  std::printf( "test_wcwidth_oracle: vendored profile matches real glibc over all 1114112 scalars\n" );
  return 0;
}
