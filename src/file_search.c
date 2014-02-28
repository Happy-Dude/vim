/* TODO: make some #ifdef for this */
/*--------[ file searching ]-------------------------------------------------*/
/*
 * File searching functions for 'path', 'tags' and 'cdpath' options.
 * External visible functions:
 * vim_findfile_init()		creates/initialises the search context
 * vim_findfile_free_visited()	free list of visited files/dirs of search
 *				context
 * vim_findfile()		find a file in the search context
 * vim_findfile_cleanup()	cleanup/free search context created by
 *				vim_findfile_init()
 *
 * All static functions and variables start with 'ff_'
 *
 * In general it works like this:
 * First you create yourself a search context by calling vim_findfile_init().
 * It is possible to give a search context from a previous call to
 * vim_findfile_init(), so it can be reused. After this you call vim_findfile()
 * until you are satisfied with the result or it returns NULL. On every call it
 * returns the next file which matches the conditions given to
 * vim_findfile_init(). If it doesn't find a next file it returns NULL.
 *
 * It is possible to call vim_findfile_init() again to reinitialise your search
 * with some new parameters. Don't forget to pass your old search context to
 * it, so it can reuse it and especially reuse the list of already visited
 * directories. If you want to delete the list of already visited directories
 * simply call vim_findfile_free_visited().
 *
 * When you are done call vim_findfile_cleanup() to free the search context.
 *
 * The function vim_findfile_init() has a long comment, which describes the
 * needed parameters.
 *
 *
 *
 * ATTENTION:
 * ==========
 *	Also we use an allocated search context here, this functions are NOT
 *	thread-safe!!!!!
 *
 *	To minimize parameter passing (or because I'm to lazy), only the
 *	external visible functions get a search context as a parameter. This is
 *	then assigned to a static global, which is used throughout the local
 *	functions.
 */

#include "vim.h"
#include "file_search.h"
#include "charset.h"
#include "fileio.h"
#include "message.h"
#include "misc1.h"
#include "misc2.h"
#include "os_unix.h"
#include "tag.h"
#include "ui.h"
#include "window.h"
#include "os/os.h"

static char_u   *ff_expand_buffer = NULL; /* used for expanding filenames */

/*
 * type for the directory search stack
 */
typedef struct ff_stack {
  struct ff_stack     *ffs_prev;

  /* the fix part (no wildcards) and the part containing the wildcards
   * of the search path
   */
  char_u              *ffs_fix_path;
  char_u              *ffs_wc_path;

  /* files/dirs found in the above directory, matched by the first wildcard
   * of wc_part
   */
  char_u              **ffs_filearray;
  int ffs_filearray_size;
  char_u ffs_filearray_cur;                  /* needed for partly handled dirs */

  /* to store status of partly handled directories
   * 0: we work on this directory for the first time
   * 1: this directory was partly searched in an earlier step
   */
  int ffs_stage;

  /* How deep are we in the directory tree?
   * Counts backward from value of level parameter to vim_findfile_init
   */
  int ffs_level;

  /* Did we already expand '**' to an empty string? */
  int ffs_star_star_empty;
} ff_stack_T;

/*
 * type for already visited directories or files.
 */
typedef struct ff_visited {
  struct ff_visited   *ffv_next;

  /* Visited directories are different if the wildcard string are
   * different. So we have to save it.
   */
  char_u              *ffv_wc_path;
  /* for unix use inode etc for comparison (needed because of links), else
   * use filename.
   */
#ifdef UNIX
  int ffv_dev_valid;                    /* ffv_dev and ffv_ino were set */
  dev_t ffv_dev;                        /* device number */
  ino_t ffv_ino;                        /* inode number */
#endif
  /* The memory for this struct is allocated according to the length of
   * ffv_fname.
   */
  char_u ffv_fname[1];                  /* actually longer */
} ff_visited_T;

/*
 * We might have to manage several visited lists during a search.
 * This is especially needed for the tags option. If tags is set to:
 *      "./++/tags,./++/TAGS,++/tags"  (replace + with *)
 * So we have to do 3 searches:
 *   1) search from the current files directory downward for the file "tags"
 *   2) search from the current files directory downward for the file "TAGS"
 *   3) search from Vims current directory downwards for the file "tags"
 * As you can see, the first and the third search are for the same file, so for
 * the third search we can use the visited list of the first search. For the
 * second search we must start from a empty visited list.
 * The struct ff_visited_list_hdr is used to manage a linked list of already
 * visited lists.
 */
typedef struct ff_visited_list_hdr {
  struct ff_visited_list_hdr  *ffvl_next;

  /* the filename the attached visited list is for */
  char_u                      *ffvl_filename;

  ff_visited_T                *ffvl_visited_list;

} ff_visited_list_hdr_T;


/*
 * '**' can be expanded to several directory levels.
 * Set the default maximum depth.
 */
#define FF_MAX_STAR_STAR_EXPAND ((char_u)30)

/*
 * The search context:
 *   ffsc_stack_ptr:	the stack for the dirs to search
 *   ffsc_visited_list: the currently active visited list
 *   ffsc_dir_visited_list: the currently active visited list for search dirs
 *   ffsc_visited_lists_list: the list of all visited lists
 *   ffsc_dir_visited_lists_list: the list of all visited lists for search dirs
 *   ffsc_file_to_search:     the file to search for
 *   ffsc_start_dir:	the starting directory, if search path was relative
 *   ffsc_fix_path:	the fix part of the given path (without wildcards)
 *			Needed for upward search.
 *   ffsc_wc_path:	the part of the given path containing wildcards
 *   ffsc_level:	how many levels of dirs to search downwards
 *   ffsc_stopdirs_v:	array of stop directories for upward search
 *   ffsc_find_what:	FINDFILE_BOTH, FINDFILE_DIR or FINDFILE_FILE
 *   ffsc_tagfile:	searching for tags file, don't use 'suffixesadd'
 */
typedef struct ff_search_ctx_T {
  ff_stack_T                 *ffsc_stack_ptr;
  ff_visited_list_hdr_T      *ffsc_visited_list;
  ff_visited_list_hdr_T      *ffsc_dir_visited_list;
  ff_visited_list_hdr_T      *ffsc_visited_lists_list;
  ff_visited_list_hdr_T      *ffsc_dir_visited_lists_list;
  char_u                     *ffsc_file_to_search;
  char_u                     *ffsc_start_dir;
  char_u                     *ffsc_fix_path;
  char_u                     *ffsc_wc_path;
  int ffsc_level;
  char_u                     **ffsc_stopdirs_v;
  int ffsc_find_what;
  int ffsc_tagfile;
} ff_search_ctx_T;

/* locally needed functions */
static int ff_check_visited __ARGS((ff_visited_T **, char_u *, char_u *));
static void vim_findfile_free_visited_list __ARGS(
    (ff_visited_list_hdr_T **list_headp));
static void ff_free_visited_list __ARGS((ff_visited_T *vl));
static ff_visited_list_hdr_T* ff_get_visited_list __ARGS(
    (char_u *, ff_visited_list_hdr_T **list_headp));
static int ff_wc_equal __ARGS((char_u *s1, char_u *s2));

static void ff_push __ARGS((ff_search_ctx_T *search_ctx, ff_stack_T *stack_ptr));
static ff_stack_T *ff_pop __ARGS((ff_search_ctx_T *search_ctx));
static void ff_clear __ARGS((ff_search_ctx_T *search_ctx));
static void ff_free_stack_element __ARGS((ff_stack_T *stack_ptr));
static ff_stack_T *ff_create_stack_element __ARGS((char_u *, char_u *, int, int));
static int ff_path_in_stoplist __ARGS((char_u *, int, char_u **));

static char_u e_pathtoolong[] = N_("E854: path too long for completion");


/*
 * Initialization routine for vim_findfile().
 *
 * Returns the newly allocated search context or NULL if an error occurred.
 *
 * Don't forget to clean up by calling vim_findfile_cleanup() if you are done
 * with the search context.
 *
 * Find the file 'filename' in the directory 'path'.
 * The parameter 'path' may contain wildcards. If so only search 'level'
 * directories deep. The parameter 'level' is the absolute maximum and is
 * not related to restricts given to the '**' wildcard. If 'level' is 100
 * and you use '**200' vim_findfile() will stop after 100 levels.
 *
 * 'filename' cannot contain wildcards!  It is used as-is, no backslashes to
 * escape special characters.
 *
 * If 'stopdirs' is not NULL and nothing is found downward, the search is
 * restarted on the next higher directory level. This is repeated until the
 * start-directory of a search is contained in 'stopdirs'. 'stopdirs' has the
 * format ";*<dirname>*\(;<dirname>\)*;\=$".
 *
 * If the 'path' is relative, the starting dir for the search is either VIM's
 * current dir or if the path starts with "./" the current files dir.
 * If the 'path' is absolute, the starting dir is that part of the path before
 * the first wildcard.
 *
 * Upward search is only done on the starting dir.
 *
 * If 'free_visited' is TRUE the list of already visited files/directories is
 * cleared. Set this to FALSE if you just want to search from another
 * directory, but want to be sure that no directory from a previous search is
 * searched again. This is useful if you search for a file at different places.
 * The list of visited files/dirs can also be cleared with the function
 * vim_findfile_free_visited().
 *
 * Set the parameter 'find_what' to FINDFILE_DIR if you want to search for
 * directories only, FINDFILE_FILE for files only, FINDFILE_BOTH for both.
 *
 * A search context returned by a previous call to vim_findfile_init() can be
 * passed in the parameter "search_ctx_arg".  This context is reused and
 * reinitialized with the new parameters.  The list of already visited
 * directories from this context is only deleted if the parameter
 * "free_visited" is true.  Be aware that the passed "search_ctx_arg" is freed
 * if the reinitialization fails.
 *
 * If you don't have a search context from a previous call "search_ctx_arg"
 * must be NULL.
 *
 * This function silently ignores a few errors, vim_findfile() will have
 * limited functionality then.
 */
void *
vim_findfile_init (
    char_u *path,
    char_u *filename,
    char_u *stopdirs,
    int level,
    int free_visited,
    int find_what,
    void *search_ctx_arg,
    int tagfile,                    /* expanding names of tags files */
    char_u *rel_fname         /* file name to use for "." */
)
{
  char_u              *wc_part;
  ff_stack_T          *sptr;
  ff_search_ctx_T     *search_ctx;

  /* If a search context is given by the caller, reuse it, else allocate a
   * new one.
   */
  if (search_ctx_arg != NULL)
    search_ctx = search_ctx_arg;
  else {
    search_ctx = (ff_search_ctx_T*)alloc((unsigned)sizeof(ff_search_ctx_T));
    if (search_ctx == NULL)
      goto error_return;
    vim_memset(search_ctx, 0, sizeof(ff_search_ctx_T));
  }
  search_ctx->ffsc_find_what = find_what;
  search_ctx->ffsc_tagfile = tagfile;

  /* clear the search context, but NOT the visited lists */
  ff_clear(search_ctx);

  /* clear visited list if wanted */
  if (free_visited == TRUE)
    vim_findfile_free_visited(search_ctx);
  else {
    /* Reuse old visited lists. Get the visited list for the given
     * filename. If no list for the current filename exists, creates a new
     * one. */
    search_ctx->ffsc_visited_list = ff_get_visited_list(filename,
        &search_ctx->ffsc_visited_lists_list);
    if (search_ctx->ffsc_visited_list == NULL)
      goto error_return;
    search_ctx->ffsc_dir_visited_list = ff_get_visited_list(filename,
        &search_ctx->ffsc_dir_visited_lists_list);
    if (search_ctx->ffsc_dir_visited_list == NULL)
      goto error_return;
  }

  if (ff_expand_buffer == NULL) {
    ff_expand_buffer = (char_u*)alloc(MAXPATHL);
    if (ff_expand_buffer == NULL)
      goto error_return;
  }

  /* Store information on starting dir now if path is relative.
   * If path is absolute, we do that later.  */
  if (path[0] == '.'
      && (vim_ispathsep(path[1]) || path[1] == NUL)
      && (!tagfile || vim_strchr(p_cpo, CPO_DOTTAG) == NULL)
      && rel_fname != NULL) {
    int len = (int)(gettail(rel_fname) - rel_fname);

    if (!vim_isAbsName(rel_fname) && len + 1 < MAXPATHL) {
      /* Make the start dir an absolute path name. */
      vim_strncpy(ff_expand_buffer, rel_fname, len);
      search_ctx->ffsc_start_dir = FullName_save(ff_expand_buffer, FALSE);
    } else
      search_ctx->ffsc_start_dir = vim_strnsave(rel_fname, len);
    if (search_ctx->ffsc_start_dir == NULL)
      goto error_return;
    if (*++path != NUL)
      ++path;
  } else if (*path == NUL || !vim_isAbsName(path))   {
#ifdef BACKSLASH_IN_FILENAME
    /* "c:dir" needs "c:" to be expanded, otherwise use current dir */
    if (*path != NUL && path[1] == ':') {
      char_u drive[3];

      drive[0] = path[0];
      drive[1] = ':';
      drive[2] = NUL;
      if (vim_FullName(drive, ff_expand_buffer, MAXPATHL, TRUE) == FAIL)
        goto error_return;
      path += 2;
    } else
#endif
    if (mch_dirname(ff_expand_buffer, MAXPATHL) == FAIL)
      goto error_return;

    search_ctx->ffsc_start_dir = vim_strsave(ff_expand_buffer);
    if (search_ctx->ffsc_start_dir == NULL)
      goto error_return;

#ifdef BACKSLASH_IN_FILENAME
    /* A path that starts with "/dir" is relative to the drive, not to the
     * directory (but not for "//machine/dir").  Only use the drive name. */
    if ((*path == '/' || *path == '\\')
        && path[1] != path[0]
        && search_ctx->ffsc_start_dir[1] == ':')
      search_ctx->ffsc_start_dir[2] = NUL;
#endif
  }

  /*
   * If stopdirs are given, split them into an array of pointers.
   * If this fails (mem allocation), there is no upward search at all or a
   * stop directory is not recognized -> continue silently.
   * If stopdirs just contains a ";" or is empty,
   * search_ctx->ffsc_stopdirs_v will only contain a  NULL pointer. This
   * is handled as unlimited upward search.  See function
   * ff_path_in_stoplist() for details.
   */
  if (stopdirs != NULL) {
    char_u  *walker = stopdirs;
    int dircount;

    while (*walker == ';')
      walker++;

    dircount = 1;
    search_ctx->ffsc_stopdirs_v =
      (char_u **)alloc((unsigned)sizeof(char_u *));

    if (search_ctx->ffsc_stopdirs_v != NULL) {
      do {
        char_u  *helper;
        void    *ptr;

        helper = walker;
        ptr = vim_realloc(search_ctx->ffsc_stopdirs_v,
            (dircount + 1) * sizeof(char_u *));
        if (ptr)
          search_ctx->ffsc_stopdirs_v = ptr;
        else
          /* ignore, keep what we have and continue */
          break;
        walker = vim_strchr(walker, ';');
        if (walker) {
          search_ctx->ffsc_stopdirs_v[dircount-1] =
            vim_strnsave(helper, (int)(walker - helper));
          walker++;
        } else
          /* this might be "", which means ascent till top
           * of directory tree.
           */
          search_ctx->ffsc_stopdirs_v[dircount-1] =
            vim_strsave(helper);

        dircount++;

      } while (walker != NULL);
      search_ctx->ffsc_stopdirs_v[dircount-1] = NULL;
    }
  }

  search_ctx->ffsc_level = level;

  /* split into:
   *  -fix path
   *  -wildcard_stuff (might be NULL)
   */
  wc_part = vim_strchr(path, '*');
  if (wc_part != NULL) {
    int llevel;
    int len;
    char    *errpt;

    /* save the fix part of the path */
    search_ctx->ffsc_fix_path = vim_strnsave(path, (int)(wc_part - path));

    /*
     * copy wc_path and add restricts to the '**' wildcard.
     * The octet after a '**' is used as a (binary) counter.
     * So '**3' is transposed to '**^C' ('^C' is ASCII value 3)
     * or '**76' is transposed to '**N'( 'N' is ASCII value 76).
     * For EBCDIC you get different character values.
     * If no restrict is given after '**' the default is used.
     * Due to this technique the path looks awful if you print it as a
     * string.
     */
    len = 0;
    while (*wc_part != NUL) {
      if (len + 5 >= MAXPATHL) {
        EMSG(_(e_pathtoolong));
        break;
      }
      if (STRNCMP(wc_part, "**", 2) == 0) {
        ff_expand_buffer[len++] = *wc_part++;
        ff_expand_buffer[len++] = *wc_part++;

        llevel = strtol((char *)wc_part, &errpt, 10);
        if ((char_u *)errpt != wc_part && llevel > 0 && llevel < 255)
          ff_expand_buffer[len++] = llevel;
        else if ((char_u *)errpt != wc_part && llevel == 0)
          /* restrict is 0 -> remove already added '**' */
          len -= 2;
        else
          ff_expand_buffer[len++] = FF_MAX_STAR_STAR_EXPAND;
        wc_part = (char_u *)errpt;
        if (*wc_part != NUL && !vim_ispathsep(*wc_part)) {
          EMSG2(_(
                  "E343: Invalid path: '**[number]' must be at the end of the path or be followed by '%s'."),
              PATHSEPSTR);
          goto error_return;
        }
      } else
        ff_expand_buffer[len++] = *wc_part++;
    }
    ff_expand_buffer[len] = NUL;
    search_ctx->ffsc_wc_path = vim_strsave(ff_expand_buffer);

    if (search_ctx->ffsc_wc_path == NULL)
      goto error_return;
  } else
    search_ctx->ffsc_fix_path = vim_strsave(path);

  if (search_ctx->ffsc_start_dir == NULL) {
    /* store the fix part as startdir.
     * This is needed if the parameter path is fully qualified.
     */
    search_ctx->ffsc_start_dir = vim_strsave(search_ctx->ffsc_fix_path);
    if (search_ctx->ffsc_start_dir == NULL)
      goto error_return;
    search_ctx->ffsc_fix_path[0] = NUL;
  }

  /* create an absolute path */
  if (STRLEN(search_ctx->ffsc_start_dir)
      + STRLEN(search_ctx->ffsc_fix_path) + 3 >= MAXPATHL) {
    EMSG(_(e_pathtoolong));
    goto error_return;
  }
  STRCPY(ff_expand_buffer, search_ctx->ffsc_start_dir);
  add_pathsep(ff_expand_buffer);
  {
    int eb_len = (int)STRLEN(ff_expand_buffer);
    char_u *buf = alloc(eb_len
        + (int)STRLEN(search_ctx->ffsc_fix_path) + 1);

    STRCPY(buf, ff_expand_buffer);
    STRCPY(buf + eb_len, search_ctx->ffsc_fix_path);
    if (mch_isdir(buf)) {
      STRCAT(ff_expand_buffer, search_ctx->ffsc_fix_path);
      add_pathsep(ff_expand_buffer);
    } else   {
      char_u *p =  gettail(search_ctx->ffsc_fix_path);
      char_u *wc_path = NULL;
      char_u *temp = NULL;
      int len = 0;

      if (p > search_ctx->ffsc_fix_path) {
        len = (int)(p - search_ctx->ffsc_fix_path) - 1;
        STRNCAT(ff_expand_buffer, search_ctx->ffsc_fix_path, len);
        add_pathsep(ff_expand_buffer);
      } else
        len = (int)STRLEN(search_ctx->ffsc_fix_path);

      if (search_ctx->ffsc_wc_path != NULL) {
        wc_path = vim_strsave(search_ctx->ffsc_wc_path);
        temp = alloc((int)(STRLEN(search_ctx->ffsc_wc_path)
                           + STRLEN(search_ctx->ffsc_fix_path + len)
                           + 1));
      }

      if (temp == NULL || wc_path == NULL) {
        vim_free(buf);
        vim_free(temp);
        vim_free(wc_path);
        goto error_return;
      }

      STRCPY(temp, search_ctx->ffsc_fix_path + len);
      STRCAT(temp, search_ctx->ffsc_wc_path);
      vim_free(search_ctx->ffsc_wc_path);
      vim_free(wc_path);
      search_ctx->ffsc_wc_path = temp;
    }
    vim_free(buf);
  }

  sptr = ff_create_stack_element(ff_expand_buffer,
      search_ctx->ffsc_wc_path,
      level, 0);

  if (sptr == NULL)
    goto error_return;

  ff_push(search_ctx, sptr);

  search_ctx->ffsc_file_to_search = vim_strsave(filename);
  if (search_ctx->ffsc_file_to_search == NULL)
    goto error_return;

  return search_ctx;

error_return:
  /*
   * We clear the search context now!
   * Even when the caller gave us a (perhaps valid) context we free it here,
   * as we might have already destroyed it.
   */
  vim_findfile_cleanup(search_ctx);
  return NULL;
}

/*
 * Get the stopdir string.  Check that ';' is not escaped.
 */
char_u *vim_findfile_stopdir(char_u *buf)
{
  char_u      *r_ptr = buf;

  while (*r_ptr != NUL && *r_ptr != ';') {
    if (r_ptr[0] == '\\' && r_ptr[1] == ';') {
      /* Overwrite the escape char,
       * use STRLEN(r_ptr) to move the trailing '\0'. */
      STRMOVE(r_ptr, r_ptr + 1);
      r_ptr++;
    }
    r_ptr++;
  }
  if (*r_ptr == ';') {
    *r_ptr = 0;
    r_ptr++;
  } else if (*r_ptr == NUL)
    r_ptr = NULL;
  return r_ptr;
}

/*
 * Clean up the given search context. Can handle a NULL pointer.
 */
void vim_findfile_cleanup(void *ctx)
{
  if (ctx == NULL)
    return;

  vim_findfile_free_visited(ctx);
  ff_clear(ctx);
  vim_free(ctx);
}

/*
 * Find a file in a search context.
 * The search context was created with vim_findfile_init() above.
 * Return a pointer to an allocated file name or NULL if nothing found.
 * To get all matching files call this function until you get NULL.
 *
 * If the passed search_context is NULL, NULL is returned.
 *
 * The search algorithm is depth first. To change this replace the
 * stack with a list (don't forget to leave partly searched directories on the
 * top of the list).
 */
char_u *vim_findfile(void *search_ctx_arg)
{
  char_u      *file_path;
  char_u      *rest_of_wildcards;
  char_u      *path_end = NULL;
  ff_stack_T  *stackp;
  int len;
  int i;
  char_u      *p;
  char_u      *suf;
  ff_search_ctx_T *search_ctx;

  if (search_ctx_arg == NULL)
    return NULL;

  search_ctx = (ff_search_ctx_T *)search_ctx_arg;

  /*
   * filepath is used as buffer for various actions and as the storage to
   * return a found filename.
   */
  if ((file_path = alloc((int)MAXPATHL)) == NULL)
    return NULL;

  /* store the end of the start dir -- needed for upward search */
  if (search_ctx->ffsc_start_dir != NULL)
    path_end = &search_ctx->ffsc_start_dir[
      STRLEN(search_ctx->ffsc_start_dir)];

  /* upward search loop */
  for (;; ) {
    /* downward search loop */
    for (;; ) {
      /* check if user user wants to stop the search*/
      ui_breakcheck();
      if (got_int)
        break;

      /* get directory to work on from stack */
      stackp = ff_pop(search_ctx);
      if (stackp == NULL)
        break;

      /*
       * TODO: decide if we leave this test in
       *
       * GOOD: don't search a directory(-tree) twice.
       * BAD:  - check linked list for every new directory entered.
       *       - check for double files also done below
       *
       * Here we check if we already searched this directory.
       * We already searched a directory if:
       * 1) The directory is the same.
       * 2) We would use the same wildcard string.
       *
       * Good if you have links on same directory via several ways
       *  or you have selfreferences in directories (e.g. SuSE Linux 6.3:
       *  /etc/rc.d/init.d is linked to /etc/rc.d -> endless loop)
       *
       * This check is only needed for directories we work on for the
       * first time (hence stackp->ff_filearray == NULL)
       */
      if (stackp->ffs_filearray == NULL
          && ff_check_visited(&search_ctx->ffsc_dir_visited_list
              ->ffvl_visited_list,
              stackp->ffs_fix_path
              , stackp->ffs_wc_path
              ) == FAIL) {
#ifdef FF_VERBOSE
        if (p_verbose >= 5) {
          verbose_enter_scroll();
          smsg((char_u *)"Already Searched: %s (%s)",
              stackp->ffs_fix_path, stackp->ffs_wc_path);
          /* don't overwrite this either */
          msg_puts((char_u *)"\n");
          verbose_leave_scroll();
        }
#endif
        ff_free_stack_element(stackp);
        continue;
      }
#ifdef FF_VERBOSE
      else if (p_verbose >= 5) {
        verbose_enter_scroll();
        smsg((char_u *)"Searching: %s (%s)",
            stackp->ffs_fix_path, stackp->ffs_wc_path);
        /* don't overwrite this either */
        msg_puts((char_u *)"\n");
        verbose_leave_scroll();
      }
#endif

      /* check depth */
      if (stackp->ffs_level <= 0) {
        ff_free_stack_element(stackp);
        continue;
      }

      file_path[0] = NUL;

      /*
       * If no filearray till now expand wildcards
       * The function expand_wildcards() can handle an array of paths
       * and all possible expands are returned in one array. We use this
       * to handle the expansion of '**' into an empty string.
       */
      if (stackp->ffs_filearray == NULL) {
        char_u *dirptrs[2];

        /* we use filepath to build the path expand_wildcards() should
         * expand.
         */
        dirptrs[0] = file_path;
        dirptrs[1] = NULL;

        /* if we have a start dir copy it in */
        if (!vim_isAbsName(stackp->ffs_fix_path)
            && search_ctx->ffsc_start_dir) {
          STRCPY(file_path, search_ctx->ffsc_start_dir);
          add_pathsep(file_path);
        }

        /* append the fix part of the search path */
        STRCAT(file_path, stackp->ffs_fix_path);
        add_pathsep(file_path);

        rest_of_wildcards = stackp->ffs_wc_path;
        if (*rest_of_wildcards != NUL) {
          len = (int)STRLEN(file_path);
          if (STRNCMP(rest_of_wildcards, "**", 2) == 0) {
            /* pointer to the restrict byte
             * The restrict byte is not a character!
             */
            p = rest_of_wildcards + 2;

            if (*p > 0) {
              (*p)--;
              file_path[len++] = '*';
            }

            if (*p == 0) {
              /* remove '**<numb> from wildcards */
              STRMOVE(rest_of_wildcards, rest_of_wildcards + 3);
            } else
              rest_of_wildcards += 3;

            if (stackp->ffs_star_star_empty == 0) {
              /* if not done before, expand '**' to empty */
              stackp->ffs_star_star_empty = 1;
              dirptrs[1] = stackp->ffs_fix_path;
            }
          }

          /*
           * Here we copy until the next path separator or the end of
           * the path. If we stop at a path separator, there is
           * still something else left. This is handled below by
           * pushing every directory returned from expand_wildcards()
           * on the stack again for further search.
           */
          while (*rest_of_wildcards
                 && !vim_ispathsep(*rest_of_wildcards))
            file_path[len++] = *rest_of_wildcards++;

          file_path[len] = NUL;
          if (vim_ispathsep(*rest_of_wildcards))
            rest_of_wildcards++;
        }

        /*
         * Expand wildcards like "*" and "$VAR".
         * If the path is a URL don't try this.
         */
        if (path_with_url(dirptrs[0])) {
          stackp->ffs_filearray = (char_u **)
                                  alloc((unsigned)sizeof(char *));
          if (stackp->ffs_filearray != NULL
              && (stackp->ffs_filearray[0]
                    = vim_strsave(dirptrs[0])) != NULL)
            stackp->ffs_filearray_size = 1;
          else
            stackp->ffs_filearray_size = 0;
        } else
          /* Add EW_NOTWILD because the expanded path may contain
           * wildcard characters that are to be taken literally.
           * This is a bit of a hack. */
          expand_wildcards((dirptrs[1] == NULL) ? 1 : 2, dirptrs,
              &stackp->ffs_filearray_size,
              &stackp->ffs_filearray,
              EW_DIR|EW_ADDSLASH|EW_SILENT|EW_NOTWILD);

        stackp->ffs_filearray_cur = 0;
        stackp->ffs_stage = 0;
      } else
        rest_of_wildcards = &stackp->ffs_wc_path[
          STRLEN(stackp->ffs_wc_path)];

      if (stackp->ffs_stage == 0) {
        /* this is the first time we work on this directory */
        if (*rest_of_wildcards == NUL) {
          /*
           * We don't have further wildcards to expand, so we have to
           * check for the final file now.
           */
          for (i = stackp->ffs_filearray_cur;
               i < stackp->ffs_filearray_size; ++i) {
            if (!path_with_url(stackp->ffs_filearray[i])
                && !mch_isdir(stackp->ffs_filearray[i]))
              continue;                 /* not a directory */

            /* prepare the filename to be checked for existence
             * below */
            STRCPY(file_path, stackp->ffs_filearray[i]);
            add_pathsep(file_path);
            STRCAT(file_path, search_ctx->ffsc_file_to_search);

            /*
             * Try without extra suffix and then with suffixes
             * from 'suffixesadd'.
             */
            len = (int)STRLEN(file_path);
            if (search_ctx->ffsc_tagfile)
              suf = (char_u *)"";
            else
              suf = curbuf->b_p_sua;
            for (;; ) {
              /* if file exists and we didn't already find it */
              if ((path_with_url(file_path)
                   || (mch_getperm(file_path) >= 0
                       && (search_ctx->ffsc_find_what
                           == FINDFILE_BOTH
                           || ((search_ctx->ffsc_find_what
                                == FINDFILE_DIR)
                               == mch_isdir(file_path)))))
#ifndef FF_VERBOSE
                  && (ff_check_visited(
                          &search_ctx->ffsc_visited_list->ffvl_visited_list,
                          file_path
                          , (char_u *)""
                          ) == OK)
#endif
                  ) {
#ifdef FF_VERBOSE
                if (ff_check_visited(
                        &search_ctx->ffsc_visited_list->ffvl_visited_list,
                        file_path
                        , (char_u *)""
                        ) == FAIL) {
                  if (p_verbose >= 5) {
                    verbose_enter_scroll();
                    smsg((char_u *)"Already: %s",
                        file_path);
                    /* don't overwrite this either */
                    msg_puts((char_u *)"\n");
                    verbose_leave_scroll();
                  }
                  continue;
                }
#endif

                /* push dir to examine rest of subdirs later */
                stackp->ffs_filearray_cur = i + 1;
                ff_push(search_ctx, stackp);

                if (!path_with_url(file_path))
                  simplify_filename(file_path);
                if (mch_dirname(ff_expand_buffer, MAXPATHL)
                    == OK) {
                  p = shorten_fname(file_path,
                      ff_expand_buffer);
                  if (p != NULL)
                    STRMOVE(file_path, p);
                }
#ifdef FF_VERBOSE
                if (p_verbose >= 5) {
                  verbose_enter_scroll();
                  smsg((char_u *)"HIT: %s", file_path);
                  /* don't overwrite this either */
                  msg_puts((char_u *)"\n");
                  verbose_leave_scroll();
                }
#endif
                return file_path;
              }

              /* Not found or found already, try next suffix. */
              if (*suf == NUL)
                break;
              copy_option_part(&suf, file_path + len,
                  MAXPATHL - len, ",");
            }
          }
        } else   {
          /*
           * still wildcards left, push the directories for further
           * search
           */
          for (i = stackp->ffs_filearray_cur;
               i < stackp->ffs_filearray_size; ++i) {
            if (!mch_isdir(stackp->ffs_filearray[i]))
              continue;                 /* not a directory */

            ff_push(search_ctx,
                ff_create_stack_element(
                    stackp->ffs_filearray[i],
                    rest_of_wildcards,
                    stackp->ffs_level - 1, 0));
          }
        }
        stackp->ffs_filearray_cur = 0;
        stackp->ffs_stage = 1;
      }

      /*
       * if wildcards contains '**' we have to descent till we reach the
       * leaves of the directory tree.
       */
      if (STRNCMP(stackp->ffs_wc_path, "**", 2) == 0) {
        for (i = stackp->ffs_filearray_cur;
             i < stackp->ffs_filearray_size; ++i) {
          if (fnamecmp(stackp->ffs_filearray[i],
                  stackp->ffs_fix_path) == 0)
            continue;             /* don't repush same directory */
          if (!mch_isdir(stackp->ffs_filearray[i]))
            continue;               /* not a directory */
          ff_push(search_ctx,
              ff_create_stack_element(stackp->ffs_filearray[i],
                  stackp->ffs_wc_path, stackp->ffs_level - 1, 1));
        }
      }

      /* we are done with the current directory */
      ff_free_stack_element(stackp);

    }

    /* If we reached this, we didn't find anything downwards.
     * Let's check if we should do an upward search.
     */
    if (search_ctx->ffsc_start_dir
        && search_ctx->ffsc_stopdirs_v != NULL && !got_int) {
      ff_stack_T  *sptr;

      /* is the last starting directory in the stop list? */
      if (ff_path_in_stoplist(search_ctx->ffsc_start_dir,
              (int)(path_end - search_ctx->ffsc_start_dir),
              search_ctx->ffsc_stopdirs_v) == TRUE)
        break;

      /* cut of last dir */
      while (path_end > search_ctx->ffsc_start_dir
             && vim_ispathsep(*path_end))
        path_end--;
      while (path_end > search_ctx->ffsc_start_dir
             && !vim_ispathsep(path_end[-1]))
        path_end--;
      *path_end = 0;
      path_end--;

      if (*search_ctx->ffsc_start_dir == 0)
        break;

      STRCPY(file_path, search_ctx->ffsc_start_dir);
      add_pathsep(file_path);
      STRCAT(file_path, search_ctx->ffsc_fix_path);

      /* create a new stack entry */
      sptr = ff_create_stack_element(file_path,
          search_ctx->ffsc_wc_path, search_ctx->ffsc_level, 0);
      if (sptr == NULL)
        break;
      ff_push(search_ctx, sptr);
    } else
      break;
  }

  vim_free(file_path);
  return NULL;
}

/*
 * Free the list of lists of visited files and directories
 * Can handle it if the passed search_context is NULL;
 */
void vim_findfile_free_visited(void *search_ctx_arg)
{
  ff_search_ctx_T *search_ctx;

  if (search_ctx_arg == NULL)
    return;

  search_ctx = (ff_search_ctx_T *)search_ctx_arg;
  vim_findfile_free_visited_list(&search_ctx->ffsc_visited_lists_list);
  vim_findfile_free_visited_list(&search_ctx->ffsc_dir_visited_lists_list);
}

static void vim_findfile_free_visited_list(ff_visited_list_hdr_T **list_headp)
{
  ff_visited_list_hdr_T *vp;

  while (*list_headp != NULL) {
    vp = (*list_headp)->ffvl_next;
    ff_free_visited_list((*list_headp)->ffvl_visited_list);

    vim_free((*list_headp)->ffvl_filename);
    vim_free(*list_headp);
    *list_headp = vp;
  }
  *list_headp = NULL;
}

static void ff_free_visited_list(ff_visited_T *vl)
{
  ff_visited_T *vp;

  while (vl != NULL) {
    vp = vl->ffv_next;
    vim_free(vl->ffv_wc_path);
    vim_free(vl);
    vl = vp;
  }
  vl = NULL;
}

/*
 * Returns the already visited list for the given filename. If none is found it
 * allocates a new one.
 */
static ff_visited_list_hdr_T *ff_get_visited_list(char_u *filename, ff_visited_list_hdr_T **list_headp)
{
  ff_visited_list_hdr_T  *retptr = NULL;

  /* check if a visited list for the given filename exists */
  if (*list_headp != NULL) {
    retptr = *list_headp;
    while (retptr != NULL) {
      if (fnamecmp(filename, retptr->ffvl_filename) == 0) {
#ifdef FF_VERBOSE
        if (p_verbose >= 5) {
          verbose_enter_scroll();
          smsg((char_u *)"ff_get_visited_list: FOUND list for %s",
              filename);
          /* don't overwrite this either */
          msg_puts((char_u *)"\n");
          verbose_leave_scroll();
        }
#endif
        return retptr;
      }
      retptr = retptr->ffvl_next;
    }
  }

#ifdef FF_VERBOSE
  if (p_verbose >= 5) {
    verbose_enter_scroll();
    smsg((char_u *)"ff_get_visited_list: new list for %s", filename);
    /* don't overwrite this either */
    msg_puts((char_u *)"\n");
    verbose_leave_scroll();
  }
#endif

  /*
   * if we reach this we didn't find a list and we have to allocate new list
   */
  retptr = (ff_visited_list_hdr_T*)alloc((unsigned)sizeof(*retptr));
  if (retptr == NULL)
    return NULL;

  retptr->ffvl_visited_list = NULL;
  retptr->ffvl_filename = vim_strsave(filename);
  if (retptr->ffvl_filename == NULL) {
    vim_free(retptr);
    return NULL;
  }
  retptr->ffvl_next = *list_headp;
  *list_headp = retptr;

  return retptr;
}

/*
 * check if two wildcard paths are equal. Returns TRUE or FALSE.
 * They are equal if:
 *  - both paths are NULL
 *  - they have the same length
 *  - char by char comparison is OK
 *  - the only differences are in the counters behind a '**', so
 *    '**\20' is equal to '**\24'
 */
static int ff_wc_equal(char_u *s1, char_u *s2)
{
  int i;
  int prev1 = NUL;
  int prev2 = NUL;

  if (s1 == s2)
    return TRUE;

  if (s1 == NULL || s2 == NULL)
    return FALSE;

  if (STRLEN(s1) != STRLEN(s2))
    return FAIL;

  for (i = 0; s1[i] != NUL && s2[i] != NUL; i += MB_PTR2LEN(s1 + i)) {
    int c1 = PTR2CHAR(s1 + i);
    int c2 = PTR2CHAR(s2 + i);

    if ((p_fic ? MB_TOLOWER(c1) != MB_TOLOWER(c2) : c1 != c2)
        && (prev1 != '*' || prev2 != '*'))
      return FAIL;
    prev2 = prev1;
    prev1 = c1;
  }
  return TRUE;
}

/*
 * maintains the list of already visited files and dirs
 * returns FAIL if the given file/dir is already in the list
 * returns OK if it is newly added
 *
 * TODO: What to do on memory allocation problems?
 *	 -> return TRUE - Better the file is found several times instead of
 *	    never.
 */
static int ff_check_visited(ff_visited_T **visited_list, char_u *fname, char_u *wc_path)
{
  ff_visited_T        *vp;
#ifdef UNIX
  struct stat st;
  int url = FALSE;
#endif

  /* For an URL we only compare the name, otherwise we compare the
   * device/inode (unix) or the full path name (not Unix). */
  if (path_with_url(fname)) {
    vim_strncpy(ff_expand_buffer, fname, MAXPATHL - 1);
#ifdef UNIX
    url = TRUE;
#endif
  } else   {
    ff_expand_buffer[0] = NUL;
#ifdef UNIX
    if (mch_stat((char *)fname, &st) < 0)
#else
    if (vim_FullName(fname, ff_expand_buffer, MAXPATHL, TRUE) == FAIL)
#endif
      return FAIL;
  }

  /* check against list of already visited files */
  for (vp = *visited_list; vp != NULL; vp = vp->ffv_next) {
    if (
#ifdef UNIX
      !url ? (vp->ffv_dev_valid && vp->ffv_dev == st.st_dev
              && vp->ffv_ino == st.st_ino)
      :
#endif
      fnamecmp(vp->ffv_fname, ff_expand_buffer) == 0
      ) {
      /* are the wildcard parts equal */
      if (ff_wc_equal(vp->ffv_wc_path, wc_path) == TRUE)
        /* already visited */
        return FAIL;
    }
  }

  /*
   * New file/dir.  Add it to the list of visited files/dirs.
   */
  vp = (ff_visited_T *)alloc((unsigned)(sizeof(ff_visited_T)
                                        + STRLEN(ff_expand_buffer)));

  if (vp != NULL) {
#ifdef UNIX
    if (!url) {
      vp->ffv_dev_valid = TRUE;
      vp->ffv_ino = st.st_ino;
      vp->ffv_dev = st.st_dev;
      vp->ffv_fname[0] = NUL;
    } else   {
      vp->ffv_dev_valid = FALSE;
#endif
    STRCPY(vp->ffv_fname, ff_expand_buffer);
#ifdef UNIX
  }
#endif
    if (wc_path != NULL)
      vp->ffv_wc_path = vim_strsave(wc_path);
    else
      vp->ffv_wc_path = NULL;

    vp->ffv_next = *visited_list;
    *visited_list = vp;
  }

  return OK;
}

/*
 * create stack element from given path pieces
 */
static ff_stack_T *ff_create_stack_element(char_u *fix_part, char_u *wc_part, int level, int star_star_empty)
{
  ff_stack_T  *new;

  new = (ff_stack_T *)alloc((unsigned)sizeof(ff_stack_T));
  if (new == NULL)
    return NULL;

  new->ffs_prev          = NULL;
  new->ffs_filearray     = NULL;
  new->ffs_filearray_size = 0;
  new->ffs_filearray_cur  = 0;
  new->ffs_stage         = 0;
  new->ffs_level         = level;
  new->ffs_star_star_empty = star_star_empty;;

  /* the following saves NULL pointer checks in vim_findfile */
  if (fix_part == NULL)
    fix_part = (char_u *)"";
  new->ffs_fix_path = vim_strsave(fix_part);

  if (wc_part == NULL)
    wc_part  = (char_u *)"";
  new->ffs_wc_path = vim_strsave(wc_part);

  if (new->ffs_fix_path == NULL
      || new->ffs_wc_path == NULL
      ) {
    ff_free_stack_element(new);
    new = NULL;
  }

  return new;
}

/*
 * Push a dir on the directory stack.
 */
static void ff_push(ff_search_ctx_T *search_ctx, ff_stack_T *stack_ptr)
{
  /* check for NULL pointer, not to return an error to the user, but
   * to prevent a crash */
  if (stack_ptr != NULL) {
    stack_ptr->ffs_prev = search_ctx->ffsc_stack_ptr;
    search_ctx->ffsc_stack_ptr = stack_ptr;
  }
}

/*
 * Pop a dir from the directory stack.
 * Returns NULL if stack is empty.
 */
static ff_stack_T *ff_pop(ff_search_ctx_T *search_ctx)
{
  ff_stack_T  *sptr;

  sptr = search_ctx->ffsc_stack_ptr;
  if (search_ctx->ffsc_stack_ptr != NULL)
    search_ctx->ffsc_stack_ptr = search_ctx->ffsc_stack_ptr->ffs_prev;

  return sptr;
}

/*
 * free the given stack element
 */
static void ff_free_stack_element(ff_stack_T *stack_ptr)
{
  /* vim_free handles possible NULL pointers */
  vim_free(stack_ptr->ffs_fix_path);
  vim_free(stack_ptr->ffs_wc_path);

  if (stack_ptr->ffs_filearray != NULL)
    FreeWild(stack_ptr->ffs_filearray_size, stack_ptr->ffs_filearray);

  vim_free(stack_ptr);
}

/*
 * Clear the search context, but NOT the visited list.
 */
static void ff_clear(ff_search_ctx_T *search_ctx)
{
  ff_stack_T   *sptr;

  /* clear up stack */
  while ((sptr = ff_pop(search_ctx)) != NULL)
    ff_free_stack_element(sptr);

  vim_free(search_ctx->ffsc_file_to_search);
  vim_free(search_ctx->ffsc_start_dir);
  vim_free(search_ctx->ffsc_fix_path);
  vim_free(search_ctx->ffsc_wc_path);

  if (search_ctx->ffsc_stopdirs_v != NULL) {
    int i = 0;

    while (search_ctx->ffsc_stopdirs_v[i] != NULL) {
      vim_free(search_ctx->ffsc_stopdirs_v[i]);
      i++;
    }
    vim_free(search_ctx->ffsc_stopdirs_v);
  }
  search_ctx->ffsc_stopdirs_v = NULL;

  /* reset everything */
  search_ctx->ffsc_file_to_search = NULL;
  search_ctx->ffsc_start_dir = NULL;
  search_ctx->ffsc_fix_path = NULL;
  search_ctx->ffsc_wc_path = NULL;
  search_ctx->ffsc_level = 0;
}

/*
 * check if the given path is in the stopdirs
 * returns TRUE if yes else FALSE
 */
static int ff_path_in_stoplist(char_u *path, int path_len, char_u **stopdirs_v)
{
  int i = 0;

  /* eat up trailing path separators, except the first */
  while (path_len > 1 && vim_ispathsep(path[path_len - 1]))
    path_len--;

  /* if no path consider it as match */
  if (path_len == 0)
    return TRUE;

  for (i = 0; stopdirs_v[i] != NULL; i++) {
    if ((int)STRLEN(stopdirs_v[i]) > path_len) {
      /* match for parent directory. So '/home' also matches
       * '/home/rks'. Check for PATHSEP in stopdirs_v[i], else
       * '/home/r' would also match '/home/rks'
       */
      if (fnamencmp(stopdirs_v[i], path, path_len) == 0
          && vim_ispathsep(stopdirs_v[i][path_len]))
        return TRUE;
    } else   {
      if (fnamecmp(stopdirs_v[i], path) == 0)
        return TRUE;
    }
  }
  return FALSE;
}

/*
 * Find the file name "ptr[len]" in the path.  Also finds directory names.
 *
 * On the first call set the parameter 'first' to TRUE to initialize
 * the search.  For repeating calls to FALSE.
 *
 * Repeating calls will return other files called 'ptr[len]' from the path.
 *
 * Only on the first call 'ptr' and 'len' are used.  For repeating calls they
 * don't need valid values.
 *
 * If nothing found on the first call the option FNAME_MESS will issue the
 * message:
 *	    'Can't find file "<file>" in path'
 * On repeating calls:
 *	    'No more file "<file>" found in path'
 *
 * options:
 * FNAME_MESS	    give error message when not found
 *
 * Uses NameBuff[]!
 *
 * Returns an allocated string for the file name.  NULL for error.
 *
 */
char_u *
find_file_in_path (
    char_u *ptr,               /* file name */
    int len,                        /* length of file name */
    int options,
    int first,                      /* use count'th matching file name */
    char_u *rel_fname         /* file name searching relative to */
)
{
  return find_file_in_path_option(ptr, len, options, first,
      *curbuf->b_p_path == NUL ? p_path : curbuf->b_p_path,
      FINDFILE_BOTH, rel_fname, curbuf->b_p_sua);
}

static char_u   *ff_file_to_find = NULL;
static void     *fdip_search_ctx = NULL;

#if defined(EXITFREE)
void free_findfile(void)                 {
  vim_free(ff_file_to_find);
  vim_findfile_cleanup(fdip_search_ctx);
  vim_free(ff_expand_buffer);
}

#endif

/*
 * Find the directory name "ptr[len]" in the path.
 *
 * options:
 * FNAME_MESS	    give error message when not found
 *
 * Uses NameBuff[]!
 *
 * Returns an allocated string for the file name.  NULL for error.
 */
char_u *
find_directory_in_path (
    char_u *ptr,               /* file name */
    int len,                        /* length of file name */
    int options,
    char_u *rel_fname         /* file name searching relative to */
)
{
  return find_file_in_path_option(ptr, len, options, TRUE, p_cdpath,
      FINDFILE_DIR, rel_fname, (char_u *)"");
}

char_u *
find_file_in_path_option (
    char_u *ptr,               /* file name */
    int len,                        /* length of file name */
    int options,
    int first,                      /* use count'th matching file name */
    char_u *path_option,       /* p_path or p_cdpath */
    int find_what,                  /* FINDFILE_FILE, _DIR or _BOTH */
    char_u *rel_fname,         /* file name we are looking relative to. */
    char_u *suffixes          /* list of suffixes, 'suffixesadd' option */
)
{
  static char_u       *dir;
  static int did_findfile_init = FALSE;
  char_u save_char;
  char_u              *file_name = NULL;
  char_u              *buf = NULL;
  int rel_to_curdir;

  if (first == TRUE) {
    /* copy file name into NameBuff, expanding environment variables */
    save_char = ptr[len];
    ptr[len] = NUL;
    expand_env(ptr, NameBuff, MAXPATHL);
    ptr[len] = save_char;

    vim_free(ff_file_to_find);
    ff_file_to_find = vim_strsave(NameBuff);
    if (ff_file_to_find == NULL) {      /* out of memory */
      file_name = NULL;
      goto theend;
    }
  }

  rel_to_curdir = (ff_file_to_find[0] == '.'
                   && (ff_file_to_find[1] == NUL
                       || vim_ispathsep(ff_file_to_find[1])
                       || (ff_file_to_find[1] == '.'
                           && (ff_file_to_find[2] == NUL
                               || vim_ispathsep(ff_file_to_find[2])))));
  if (vim_isAbsName(ff_file_to_find)
      /* "..", "../path", "." and "./path": don't use the path_option */
      || rel_to_curdir
      ) {
    /*
     * Absolute path, no need to use "path_option".
     * If this is not a first call, return NULL.  We already returned a
     * filename on the first call.
     */
    if (first == TRUE) {
      int l;
      int run;

      if (path_with_url(ff_file_to_find)) {
        file_name = vim_strsave(ff_file_to_find);
        goto theend;
      }

      /* When FNAME_REL flag given first use the directory of the file.
       * Otherwise or when this fails use the current directory. */
      for (run = 1; run <= 2; ++run) {
        l = (int)STRLEN(ff_file_to_find);
        if (run == 1
            && rel_to_curdir
            && (options & FNAME_REL)
            && rel_fname != NULL
            && STRLEN(rel_fname) + l < MAXPATHL) {
          STRCPY(NameBuff, rel_fname);
          STRCPY(gettail(NameBuff), ff_file_to_find);
          l = (int)STRLEN(NameBuff);
        } else   {
          STRCPY(NameBuff, ff_file_to_find);
          run = 2;
        }

        /* When the file doesn't exist, try adding parts of
         * 'suffixesadd'. */
        buf = suffixes;
        for (;; ) {
          if (
            (mch_getperm(NameBuff) >= 0
             && (find_what == FINDFILE_BOTH
                 || ((find_what == FINDFILE_DIR)
                     == mch_isdir(NameBuff))))) {
            file_name = vim_strsave(NameBuff);
            goto theend;
          }
          if (*buf == NUL)
            break;
          copy_option_part(&buf, NameBuff + l, MAXPATHL - l, ",");
        }
      }
    }
  } else   {
    /*
     * Loop over all paths in the 'path' or 'cdpath' option.
     * When "first" is set, first setup to the start of the option.
     * Otherwise continue to find the next match.
     */
    if (first == TRUE) {
      /* vim_findfile_free_visited can handle a possible NULL pointer */
      vim_findfile_free_visited(fdip_search_ctx);
      dir = path_option;
      did_findfile_init = FALSE;
    }

    for (;; ) {
      if (did_findfile_init) {
        file_name = vim_findfile(fdip_search_ctx);
        if (file_name != NULL)
          break;

        did_findfile_init = FALSE;
      } else   {
        char_u  *r_ptr;

        if (dir == NULL || *dir == NUL) {
          /* We searched all paths of the option, now we can
           * free the search context. */
          vim_findfile_cleanup(fdip_search_ctx);
          fdip_search_ctx = NULL;
          break;
        }

        if ((buf = alloc((int)(MAXPATHL))) == NULL)
          break;

        /* copy next path */
        buf[0] = 0;
        copy_option_part(&dir, buf, MAXPATHL, " ,");

        /* get the stopdir string */
        r_ptr = vim_findfile_stopdir(buf);
        fdip_search_ctx = vim_findfile_init(buf, ff_file_to_find,
            r_ptr, 100, FALSE, find_what,
            fdip_search_ctx, FALSE, rel_fname);
        if (fdip_search_ctx != NULL)
          did_findfile_init = TRUE;
        vim_free(buf);
      }
    }
  }
  if (file_name == NULL && (options & FNAME_MESS)) {
    if (first == TRUE) {
      if (find_what == FINDFILE_DIR)
        EMSG2(_("E344: Can't find directory \"%s\" in cdpath"),
            ff_file_to_find);
      else
        EMSG2(_("E345: Can't find file \"%s\" in path"),
            ff_file_to_find);
    } else   {
      if (find_what == FINDFILE_DIR)
        EMSG2(_("E346: No more directory \"%s\" found in cdpath"),
            ff_file_to_find);
      else
        EMSG2(_("E347: No more file \"%s\" found in path"),
            ff_file_to_find);
    }
  }

theend:
  return file_name;
}

