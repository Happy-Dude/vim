/* vi:set ts=8 sts=4 sw=4:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * fs.c -- filesystem access
 */

#include <uv.h>

#include "os.h"

int mch_chdir(char *path) {
  if (p_verbose >= 5) {
    verbose_enter();
    smsg((char_u *)"chdir(%s)", path);
    verbose_leave();
  }
  return uv_chdir(path);
}

/*
 * Get name of current directory into buffer 'buf' of length 'len' bytes.
 * Return OK for success, FAIL for failure.
 */
int mch_dirname(char_u *buf, int len)
{
  int errno;
  if ((errno = uv_cwd((char *)buf, len)) != 0) {
      STRCPY(buf, uv_strerror(errno));
      return FAIL;
  }
  return OK;
}
