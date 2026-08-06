/* ABOUTME: Pure parsers for mosh client startup configuration environment variables. */
/* ABOUTME: No OS calls; shared by the POSIX STMClient frontend and the Windows MoshCore. */

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
    file(s), but you are not obligated to do so. If you do not wish
    to do so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

#ifndef MOSH_STARTUP_CONFIG_H
#define MOSH_STARTUP_CONFIG_H

#include <string>

#include "src/frontend/terminaloverlay.h"

/* Parsed escape-key configuration. Defaults match mosh's historical Ctrl-^. */
struct EscapeConfig
{
  int key = 0x1e;
  int pass_key = '^';
  int pass_key2 = '^';
  bool requires_lf = false;
};

/* Parse MOSH_ESCAPE_KEY (env may be NULL). One ASCII key in 1-127; an empty
   string disables the parser (key == -1); a control key passes with the
   un-control key and needs no line start; a printable key passes itself and
   requires line start; a forbidden control (0x03/0x04/0x0A/0x0C/0x0D) resets
   to the default. */
EscapeConfig parse_escape_key( const char* env );

/* The display spellings for the escape help string: pass_name is the "%c"
   spelling of the literal-pass key; key_name is "Ctrl-%c" for a control escape
   or "%c" for a printable one. Callers assemble their own help sentence around
   these (the surrounding wording differs by frontend). */
void escape_key_names( const EscapeConfig& cfg, std::string* pass_name, std::string* key_name );

/* Parse MOSH_PREDICTION_DISPLAY (env may be NULL → Adaptive). Accepts exactly
   adaptive/always/never/experimental. Returns false and sets *error on any
   other value, the empty string included; *out is left unchanged in that case. */
bool parse_prediction_display( const char* env,
                               Overlay::PredictionEngine::DisplayPreference* out,
                               std::string* error );

/* True only when MOSH_PREDICTION_OVERWRITE is exactly "yes". */
bool parse_prediction_overwrite( const char* env );

/* True when the [mosh] title prefix should be applied: MOSH_TITLE_NOPREFIX
   absent (env == NULL). Any set value, empty included, suppresses it. */
bool wants_title_prefix( const char* env );

#endif
