/* vi:set ts=2 sts=2 sw=2:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * users.c -- operating system user information
 */

#include <uv.h>

#include "os.h"
#include "../garray.h"
#include "../misc2.h"
#ifdef HAVE_PWD_H
# include <pwd.h>
#endif

/*
 * Initialize users garray and fill it with os usernames.
 * Return Ok for success, FAIL for failure.
 */
int mch_get_usernames(garray_T *users)
{
  if (users == NULL) {
    return FALSE;
  }
  ga_init2(users, sizeof(char *), 20);

# if defined(HAVE_GETPWENT) && defined(HAVE_PWD_H)
  char *user;
  struct passwd *pw;

  setpwent();
  while ((pw = getpwent()) != NULL) {
    /* pw->pw_name shouldn't be NULL but just in case... */
    if (pw->pw_name != NULL) {
      if (ga_grow(users, 1) == FAIL) {
        return FAIL;
      }
      user = (char *)vim_strsave((char_u*)pw->pw_name);
      if (user == NULL) {
        return FAIL;
      }
      ((char **)(users->ga_data))[users->ga_len++] = user;
    }
  }
  endpwent();
# endif

  return OK;
}

/*
 * Insert user name in s[len].
 * Return OK if a name found.
 */
int mch_get_user_name(char *s, size_t len)
{
  return mch_get_uname(getuid(), s, len);
}

/*
 * Insert user name for "uid" in s[len].
 * Return OK if a name found.
 * If the name is not found, write the uid into s[len] and return FAIL.
 */
int mch_get_uname(uid_t uid, char *s, size_t len)
{
#if defined(HAVE_PWD_H) && defined(HAVE_GETPWUID)
  struct passwd *pw;

  if ((pw = getpwuid(uid)) != NULL
      && pw->pw_name != NULL && *(pw->pw_name) != NUL) {
    vim_strncpy((char_u *)s, (char_u *)pw->pw_name, len - 1);
    return OK;
  }
#endif
  snprintf(s, len, "%d", (int)uid);
  return FAIL; // a number is not a name
}

/*
 * Returns the user directory for the given username.
 * The caller has to free() the returned string.
 * If the username is not found, NULL is returned.
 */
char *mch_get_user_directory(const char *name)
{
#if defined(HAVE_GETPWNAM) && defined(HAVE_PWD_H)
  struct passwd *pw;
  if (name == NULL) {
    return NULL;
  }
  pw = getpwnam(name);
  if (pw != NULL) {
    // save the string from the static passwd entry into malloced memory
    char *user_directory = (char *)vim_strsave((char_u *)pw->pw_dir);
    return user_directory;
  }
#endif
  return NULL;
}

