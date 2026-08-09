/* ABOUTME: Generate the astral wcwidth interval table from glibc. */
/* ABOUTME: The probe is pinned to the C.UTF-8 locale used by glibc 2.39. */
#define _GNU_SOURCE
#include <gnu/libc-version.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int width_for( unsigned int scalar )
{
  return wcwidth( scalar );
}

static void emit_ranges( void )
{
  unsigned int first = 0;
  unsigned int last = 0;
  int previous = 1;

  for ( unsigned int scalar = 0x10000; scalar <= 0x10ffff; ++scalar ) {
    const int width = width_for( scalar );
    if ( width != previous ) {
      if ( previous != 1 ) {
        printf( "    { 0x%05x, 0x%05x, %d },\n", first, last, previous );
      }
      first = scalar;
      previous = width;
    }
    last = scalar;
  }
  if ( previous != 1 ) {
    printf( "    { 0x%05x, 0x%05x, %d },\n", first, last, previous );
  }
}

int main( void )
{
  /* The emitted header claims glibc 2.39 provenance; refuse to run under any
     other libc or version rather than silently emit a mislabeled table. */
  if ( strcmp( gnu_get_libc_version(), "2.39" ) != 0 ) {
    fprintf( stderr, "gen_wcwidth_tables: pinned oracle is glibc 2.39, but this host has glibc %s\n",
             gnu_get_libc_version() );
    return 1;
  }
  if ( !setlocale( LC_ALL, "C.UTF-8" ) ) {
    fprintf( stderr, "gen_wcwidth_tables: failed to set locale C.UTF-8\n" );
    return 1;
  }

  puts( "/* BEGIN GENERATED ASTRAL WIDTH TABLES: glibc 2.39 C.UTF-8. */" );
  emit_ranges();
  puts( "/* END GENERATED ASTRAL WIDTH TABLES. */" );
  return 0;
}
