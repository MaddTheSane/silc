/*

  silcbeosutil.c 

  Author: Pekka Riikonen <priikone@silcnet.org>

  Copyright (C) 2002 Pekka Riikonen

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

*/
/* $Id$ */

#include "silc.h"

/* Inspects the `string' for wildcards and returns regex string that can
   be used by the GNU regex library. A comma (`,') in the `string' means
   that the string is list. */

char *silc_string_regexify(const char *string)
{
  int i, len, count;
  char *regex;

  len = strlen(string);
  count = 4;
  for (i = 0; i < len; i++)
    if (string[i] == '*' || string[i] == '?')
      count++;

  regex = silc_calloc(len + count, sizeof(*regex));

  count = 0;
  regex[count] = '(';
  count++;

  for (i = 0; i < len; i++) {
    if (string[i] == '*' || string[i] == '?') {
      regex[count] = '.';
      count++;
    } else if (string[i] == ',') {
      if (i + 1 == len)
	continue;
      regex[count] = '|';
      count++;
      continue;
    }

    regex[count] = string[i];
    count++;
  }

  regex[count++] = ')';
  regex[count] = '$';

  return regex;
}

/* Combines two regex strings into one regex string so that they can be
   used as one by the GNU regex library. The `string2' is combine into
   the `string1'. */

char *silc_string_regex_combine(const char *string1, const char *string2)
{
  char *tmp;
  int len1, len2;

  len1 = strlen(string1);
  len2 = strlen(string2);

  tmp = silc_calloc(2 + len1 + len2, sizeof(*tmp));
  strncat(tmp, string1, len1 - 2);
  strncat(tmp, "|", 1);
  strncat(tmp, string2 + 1, len2 - 1);

  return tmp;
}

/* Matches the two strings and returns TRUE if the strings match. */

int silc_string_regex_match(const char *regex, const char *string)
{
  regex_t preg;
  int ret = FALSE;
  
  if (regcomp(&preg, regex, REG_NOSUB | REG_EXTENDED) < 0)
    return FALSE;

  if (regexec(&preg, string, 0, NULL, 0) == 0)
    ret = TRUE;

  regfree(&preg);

  return ret;
}

/* Do regex match to the two strings `string1' and `string2'. If the
   `string2' matches the `string1' this returns TRUE. */

int silc_string_match(const char *string1, const char *string2)
{
  char *s1;
  int ret = FALSE;

  if (!string1 || !string2)
    return ret;

  s1 = silc_string_regexify(string1);
  ret = silc_string_regex_match(s1, string2);
  silc_free(s1);

  return ret;
}

/* Return current time to struct timeval. */

int silc_gettimeofday(struct timeval *p)
{
  return gettimeofday(p, NULL);
}
