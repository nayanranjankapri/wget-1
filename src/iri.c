/* IRI related functions.
   Copyright (C) 2008 Free Software Foundation, Inc.

This file is part of GNU Wget.

GNU Wget is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or (at
your option) any later version.

GNU Wget is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Wget.  If not, see <http://www.gnu.org/licenses/>.

Additional permission under GNU GPL version 3 section 7

If you modify this program, or any covered work, by linking or
combining it with the OpenSSL project's OpenSSL library (or a
modified version of that library), containing parts covered by the
terms of the OpenSSL or SSLeay licenses, the Free Software Foundation
grants you additional permission to convey the resulting work.
Corresponding Source for a non-source form of such a combination
shall include the source code for the parts of OpenSSL used as well
as that of the covered work.  */

#include "wget.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <iconv.h>
#include <stringprep.h>
#include <idna.h>
#include <errno.h>

#include "utils.h"
#include "iri.h"

/* Note: locale encoding is kept in options struct (opt.locale) */

/* Hold the encoding used for the current fetch */
char *remote;

/* Hold the encoding for the future found links */
char *current;

/* Will/Is the current URL encoded in utf8 ? */
bool utf8_encode;

/* Force no utf8 encoding for url_parse () */
bool ugly_no_encode;

static iconv_t locale2utf8;

static bool open_locale_to_utf8 (void);
static bool do_conversion (iconv_t cd, char *in, size_t inlen, char **out);


/* Given a string containing "charset=XXX", return the encoding if found,
   or NULL otherwise */
char *
parse_charset (char *str)
{
  char *charset;

  if (!str || !*str)
    return NULL;

  str = strcasestr (str, "charset=");
  if (!str)
    return NULL;

  str += 8;
  charset = str;

  /* sXXXav: which chars should be banned ??? */
  while (*charset && !c_isspace (*charset))
    charset++;

  /* sXXXav: could strdupdelim return NULL ? */
  charset = strdupdelim (str, charset);

  /* Do a minimum check on the charset value */
  if (!check_encoding_name (charset))
    {
      xfree (charset);
      return NULL;
    }

  /*logprintf (LOG_VERBOSE, "parse_charset: %s\n", quote (charset));*/

  return charset;
}

/* Find the locale used, or fall back on a default value */
char *
find_locale (void)
{
  return (char *) stringprep_locale_charset ();
}

/* Basic check of an encoding name. */
bool
check_encoding_name (char *encoding)
{
  char *s = encoding;

  while (*s)
    {
      if (!c_isascii (*s) || c_isspace (*s))
        {
          logprintf (LOG_VERBOSE, "Encoding %s isn't valid\n", quote (encoding));
          return false;
        }

      s++;
    }

  return true;
}

/* Try opening an iconv_t descriptor for conversion from locale to UTF-8 */
static bool
open_locale_to_utf8 (void)
{
  if (locale2utf8)
    return true;

  /* sXXXav : That shouldn't happen, just in case */
  if (!opt.locale)
    {
      logprintf (LOG_VERBOSE, "open_locale_to_utf8: locale is unset\n");
      opt.locale = find_locale ();
    }

  if (!opt.locale)
    return false;

  locale2utf8 = iconv_open ("UTF-8", opt.locale);
  if (locale2utf8 != (iconv_t)(-1))
    return true;

  logprintf (LOG_VERBOSE, "Conversion from %s to %s isn't supported\n",
             quote (opt.locale), quote ("UTF-8"));
  locale2utf8 = NULL;
  return false;
}

/* Try converting string str from locale to UTF-8. Return a new string
   on success, or str on error or if conversion isn't needed. */
const char *
locale_to_utf8 (const char *str)
{
  char *new;

  if (!strcasecmp (opt.locale, "utf-8"))
    return str;

  if (!open_locale_to_utf8 ())
    return str;

  if (do_conversion (locale2utf8, (char *) str, strlen ((char *) str), &new))
    return (const char *) new;

  return str;
}

/* Do the conversion according to the passed conversion descriptor cd. *out
   will containes the transcoded string on success. *out content is
   unspecified otherwise. */
static bool
do_conversion (iconv_t cd, char *in, size_t inlen, char **out)
{
  /* sXXXav : hummm hard to guess... */
  size_t len, done, outlen = inlen * 2;
  int invalid = 0, tooshort = 0;
  char *s;

  s = xmalloc (outlen + 1);
  *out = s;
  len = outlen;
  done = 0;

  for (;;)
    {
      if (iconv (cd, &in, &inlen, out, &outlen) != (size_t)(-1))
        {
          *out = s;
          *(s + len - outlen - done) = '\0';
          return true;
        }

      /* Incomplete or invalid multibyte sequence */
      if (errno == EINVAL || errno == EILSEQ)
        {
          if (!invalid)
            logprintf (LOG_VERBOSE,
                      "Incomplete or invalide multibyte sequence encountered\n");

          invalid++;
          **out = *in;
          in++;
          inlen--;
          (*out)++;
          outlen--;
        }
      else if (errno == E2BIG) /* Output buffer full */
        {
          char *new;

          tooshort++;
          done = len;
          outlen = done + inlen * 2;
          new = xmalloc (outlen + 1);
          memcpy (new, s, done);
          xfree (s);
          s = new;
          len = outlen;
          *out = s + done;
        }
      else /* Weird, we got an unspecified error */
        {
          logprintf (LOG_VERBOSE, "Unhandled errno %d\n", errno);
          break;
        }
    }

    return false;
}

/* Try to "ASCII encode" UTF-8 host. Return the new domain on success or NULL
   on error. */
char *
idn_encode (char *host, bool utf8_encoded)
{
  char *new;
  int ret;

  /* Encode to UTF-8 if not done using current remote */
  if (!utf8_encoded)
    {
      if (!remote_to_utf8 ((const char *) host, (const char **) &new))
        {
          /* Nothing to encode or an error occured */
          return NULL;
        }

      host = new;
    }

  /* toASCII UTF-8 NULL terminated string */
  ret = idna_to_ascii_8z (host, &new, 0);
  if (ret != IDNA_SUCCESS)
    {
      /* sXXXav : free new when needed ! */
      logprintf (LOG_VERBOSE, "idn_encode failed (%d): %s\n", ret,
                 quote (idna_strerror (ret)));
      return NULL;
    }

  return new;
}

/* Try to decode an "ASCII encoded" host. Return the new domain in the locale
   on success or NULL on error. */
char *
idn_decode (char *host)
{
  char *new;
  int ret;

  ret = idna_to_unicode_8zlz (host, &new, 0);
  if (ret != IDNA_SUCCESS)
    {
      logprintf (LOG_VERBOSE, "idn_decode failed (%d): %s\n", ret,
                 quote (idna_strerror (ret)));
      return NULL;
    }

  return new;
}

/* Try to transcode string str from remote encoding to UTF-8. On success, *new
   contains the transcoded string. *new content is unspecified otherwise. */
bool
remote_to_utf8 (const char *str, const char **new)
{
  char *r;
  iconv_t cd;
  bool ret = false;

  if (opt.encoding_remote)
    r = opt.encoding_remote;
  else if (current)
    r = current;
  else
    return false;

  cd = iconv_open ("UTF-8", r);
  if (cd == (iconv_t)(-1))
    return false;

  if (do_conversion (cd, (char *) str, strlen ((char *) str), (char **) new))
    ret = true;

  iconv_close (cd);

  /* Test if something was converted */
  if (!strcmp (str, *new))
    {
      xfree ((char *) *new);
      return false;
    }

  return ret;
}

char *get_remote_charset (void)
{
  return remote;
}

char *get_current_charset (void)
{
  return current;
}

void set_current_charset (char *charset)
{
  /*printf("[ current = `%s'\n", charset);*/
  if (current)
    xfree (current);

  current = charset ? xstrdup (charset) : NULL;
}

void set_current_as_locale (void)
{
  /*printf("[ current = locale = `%s'\n", opt.locale);*/
  if (current)
    xfree (current);

  /* sXXXav : assert opt.locale NULL ? */
  current = xstrdup (opt.locale);
}

void
set_remote_charset (char *charset)
{
  /*printf("[ remote = `%s'\n", charset);*/
  if (remote)
    xfree (remote);

  remote = charset ? xstrdup (charset) : NULL;
}

void
set_remote_as_current (void)
{
  /*printf("[ remote = current = `%s'\n", current);*/
  if (remote)
    xfree (remote);

  remote = current ? xstrdup (current) : NULL;
}

void reset_utf8_encode (void)
{
  set_utf8_encode (opt.enable_iri);
}

void set_utf8_encode (bool encode)
{
  utf8_encode = encode;
}

bool get_utf8_encode (void)
{
  return utf8_encode;
}

void set_ugly_no_encode (bool ugly)
{
  ugly_no_encode = ugly;
}

bool get_ugly_no_encode (void)
{
  return ugly_no_encode;
}

