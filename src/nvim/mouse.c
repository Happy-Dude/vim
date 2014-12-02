#include <stdbool.h>

#include "nvim/mouse.h"
#include "nvim/vim.h"
#include "nvim/ascii.h"
#include "nvim/window.h"
#include "nvim/strings.h"
#include "nvim/screen.h"
#include "nvim/ui.h"
#include "nvim/os_unix.h"
#include "nvim/term.h"
#include "nvim/fold.h"
#include "nvim/diff.h"
#include "nvim/move.h"
#include "nvim/misc1.h"
#include "nvim/cursor.h"
#include "nvim/buffer_defs.h"

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "mouse.c.generated.h"
#endif

// Move the cursor to the specified row and column on the screen.
// Change current window if necessary. Returns an integer with the
// CURSOR_MOVED bit set if the cursor has moved or unset otherwise.
//
// The MOUSE_FOLD_CLOSE bit is set when clicked on the '-' in a fold column.
// The MOUSE_FOLD_OPEN bit is set when clicked on the '+' in a fold column.
//
// If flags has MOUSE_FOCUS, then the current window will not be changed, and
// if the mouse is outside the window then the text will scroll, or if the
// mouse was previously on a status line, then the status line may be dragged.
//
// If flags has MOUSE_MAY_VIS, then VIsual mode will be started before the
// cursor is moved unless the cursor was on a status line.
// This function returns one of IN_UNKNOWN, IN_BUFFER, IN_STATUS_LINE or
// IN_SEP_LINE depending on where the cursor was clicked.
//
// If flags has MOUSE_MAY_STOP_VIS, then Visual mode will be stopped, unless
// the mouse is on the status line of the same window.
//
// If flags has MOUSE_DID_MOVE, nothing is done if the mouse didn't move since
// the last call.
//
// If flags has MOUSE_SETPOS, nothing is done, only the current position is
// remembered.
int jump_to_mouse(int flags,
                  bool *inclusive,  // used for inclusive operator, can be NULL
                  int which_button)  // MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE
{
  static int on_status_line = 0;        // #lines below bottom of window
  static int on_sep_line = 0;           // on separator right of window
  static int prev_row = -1;
  static int prev_col = -1;
  static win_T *dragwin = NULL;         // window being dragged
  static int did_drag = false;          // drag was noticed

  win_T       *wp, *old_curwin;
  pos_T old_cursor;
  int count;
  bool first;
  int row = mouse_row;
  int col = mouse_col;
  int mouse_char;

  mouse_past_bottom = false;
  mouse_past_eol = false;

  if (flags & MOUSE_RELEASED) {
    // On button release we may change window focus if positioned on a
    // status line and no dragging happened.
    if (dragwin != NULL && !did_drag)
      flags &= ~(MOUSE_FOCUS | MOUSE_DID_MOVE);
    dragwin = NULL;
    did_drag = false;
  }

  if ((flags & MOUSE_DID_MOVE)
      && prev_row == mouse_row
      && prev_col == mouse_col) {
retnomove:
    // before moving the cursor for a left click which is NOT in a status
    // line, stop Visual mode
    if (on_status_line)
      return IN_STATUS_LINE;
    if (on_sep_line)
      return IN_SEP_LINE;
    if (flags & MOUSE_MAY_STOP_VIS) {
      end_visual_mode();
      redraw_curbuf_later(INVERTED);            // delete the inversion
    }
    return IN_BUFFER;
  }

  prev_row = mouse_row;
  prev_col = mouse_col;

  if (flags & MOUSE_SETPOS)
    goto retnomove;                             // ugly goto...

  // Remember the character under the mouse, it might be a '-' or '+' in the
  // fold column.
  if (row >= 0 && row < Rows && col >= 0 && col <= Columns
      && ScreenLines != NULL)
    mouse_char = ScreenLines[LineOffset[row] + (unsigned)col];
  else
    mouse_char = ' ';

  old_curwin = curwin;
  old_cursor = curwin->w_cursor;

  if (!(flags & MOUSE_FOCUS)) {
    if (row < 0 || col < 0)                     // check if it makes sense
      return IN_UNKNOWN;

    // find the window where the row is in
    wp = mouse_find_win(&row, &col);
    dragwin = NULL;
    // winpos and height may change in win_enter()!
    if (row >= wp->w_height) {                  // In (or below) status line
      on_status_line = row - wp->w_height + 1;
      dragwin = wp;
    } else {
      on_status_line = 0;
    }

    if (col >= wp->w_width) {           // In separator line
      on_sep_line = col - wp->w_width + 1;
      dragwin = wp;
    } else {
      on_sep_line = 0;
    }

    // The rightmost character of the status line might be a vertical
    // separator character if there is no connecting window to the right.
    if (on_status_line && on_sep_line) {
      if (stl_connected(wp))
        on_sep_line = 0;
      else
        on_status_line = 0;
    }

    // Before jumping to another buffer, or moving the cursor for a left
    // click, stop Visual mode.
    if (VIsual_active
        && (wp->w_buffer != curwin->w_buffer
            || (!on_status_line
                && !on_sep_line
                && (
                  wp->w_p_rl ? col < wp->w_width - wp->w_p_fdc :
                                     col >= wp->w_p_fdc
                                             + (cmdwin_type == 0 && wp ==
                                                curwin ? 0 : 1)
                  )
                && (flags & MOUSE_MAY_STOP_VIS)))) {
      end_visual_mode();
      redraw_curbuf_later(INVERTED);            // delete the inversion
    }
    if (cmdwin_type != 0 && wp != curwin) {
      // A click outside the command-line window: Use modeless
      // selection if possible.  Allow dragging the status lines.
      on_sep_line = 0;
      row = 0;
      col += wp->w_wincol;
      wp = curwin;
    }
    // Only change window focus when not clicking on or dragging the
    // status line.  Do change focus when releasing the mouse button
    // (MOUSE_FOCUS was set above if we dragged first).
    if (dragwin == NULL || (flags & MOUSE_RELEASED))
      win_enter(wp, true);                      // can make wp invalid!
# ifdef CHECK_DOUBLE_CLICK
    // set topline, to be able to check for double click ourselves
    if (curwin != old_curwin)
      set_mouse_topline(curwin);
# endif
    if (on_status_line) {                       // In (or below) status line
      // Don't use start_arrow() if we're in the same window
      if (curwin == old_curwin)
        return IN_STATUS_LINE;
      else
        return IN_STATUS_LINE | CURSOR_MOVED;
    }
    if (on_sep_line) {                          // In (or below) status line
      // Don't use start_arrow() if we're in the same window
      if (curwin == old_curwin)
        return IN_SEP_LINE;
      else
        return IN_SEP_LINE | CURSOR_MOVED;
    }

    curwin->w_cursor.lnum = curwin->w_topline;
  } else if (on_status_line && which_button == MOUSE_LEFT)   {
    if (dragwin != NULL) {
      // Drag the status line
      count = row - dragwin->w_winrow - dragwin->w_height + 1
              - on_status_line;
      win_drag_status_line(dragwin, count);
      did_drag |= count;
    }
    return IN_STATUS_LINE;                      // Cursor didn't move
  } else if (on_sep_line && which_button == MOUSE_LEFT)   {
    if (dragwin != NULL) {
      // Drag the separator column
      count = col - dragwin->w_wincol - dragwin->w_width + 1
              - on_sep_line;
      win_drag_vsep_line(dragwin, count);
      did_drag |= count;
    }
    return IN_SEP_LINE;                         // Cursor didn't move
  } else {
    // keep_window_focus must be true
    // before moving the cursor for a left click, stop Visual mode
    if (flags & MOUSE_MAY_STOP_VIS) {
      end_visual_mode();
      redraw_curbuf_later(INVERTED);            // delete the inversion
    }


    row -= curwin->w_winrow;
    col -= curwin->w_wincol;

    // When clicking beyond the end of the window, scroll the screen.
    // Scroll by however many rows outside the window we are.
    if (row < 0) {
      count = 0;
      for (first = true; curwin->w_topline > 1; ) {
        if (curwin->w_topfill < diff_check(curwin, curwin->w_topline))
          ++count;
        else
          count += plines(curwin->w_topline - 1);
        if (!first && count > -row)
          break;
        first = false;
        hasFolding(curwin->w_topline, &curwin->w_topline, NULL);
        if (curwin->w_topfill < diff_check(curwin, curwin->w_topline)) {
          ++curwin->w_topfill;
        } else {
          --curwin->w_topline;
          curwin->w_topfill = 0;
        }
      }
      check_topfill(curwin, false);
      curwin->w_valid &=
        ~(VALID_WROW|VALID_CROW|VALID_BOTLINE|VALID_BOTLINE_AP);
      redraw_later(VALID);
      row = 0;
    } else if (row >= curwin->w_height)   {
      count = 0;
      for (first = true; curwin->w_topline < curbuf->b_ml.ml_line_count; ) {
        if (curwin->w_topfill > 0) {
          ++count;
        } else {
          count += plines(curwin->w_topline);
        }

        if (!first && count > row - curwin->w_height + 1) {
          break;
        }
        first = false;

        if (hasFolding(curwin->w_topline, NULL, &curwin->w_topline)
            && curwin->w_topline == curbuf->b_ml.ml_line_count) {
          break;
        }

        if (curwin->w_topfill > 0) {
          --curwin->w_topfill;
        } else {
          ++curwin->w_topline;
          curwin->w_topfill =
            diff_check_fill(curwin, curwin->w_topline);
        }
      }
      check_topfill(curwin, false);
      redraw_later(VALID);
      curwin->w_valid &=
        ~(VALID_WROW|VALID_CROW|VALID_BOTLINE|VALID_BOTLINE_AP);
      row = curwin->w_height - 1;
    } else if (row == 0)   {
      // When dragging the mouse, while the text has been scrolled up as
      // far as it goes, moving the mouse in the top line should scroll
      // the text down (done later when recomputing w_topline).
      if (mouse_dragging > 0
          && curwin->w_cursor.lnum
          == curwin->w_buffer->b_ml.ml_line_count
          && curwin->w_cursor.lnum == curwin->w_topline) {
        curwin->w_valid &= ~(VALID_TOPLINE);
      }
    }
  }

  // Check for position outside of the fold column.
  if (curwin->w_p_rl ? col < curwin->w_width - curwin->w_p_fdc :
      col >= curwin->w_p_fdc + (cmdwin_type == 0 ? 0 : 1)) {
    mouse_char = ' ';
  }

  // compute the position in the buffer line from the posn on the screen
  if (mouse_comp_pos(curwin, &row, &col, &curwin->w_cursor.lnum)) {
    mouse_past_bottom = true;
  }

  // Start Visual mode before coladvance(), for when 'sel' != "old"
  if ((flags & MOUSE_MAY_VIS) && !VIsual_active) {
    check_visual_highlight();
    VIsual = old_cursor;
    VIsual_active = true;
    VIsual_reselect = true;
    // if 'selectmode' contains "mouse", start Select mode
    may_start_select('o');
    setmouse();

    if (p_smd && msg_silent == 0) {
      redraw_cmdline = true;            // show visual mode later
    }
  }

  curwin->w_curswant = col;
  curwin->w_set_curswant = false;       // May still have been true
  if (coladvance(col) == FAIL) {        // Mouse click beyond end of line
    if (inclusive != NULL) {
      *inclusive = true;
    }
    mouse_past_eol = true;
  } else if (inclusive != NULL) {
    *inclusive = false;
  }

  count = IN_BUFFER;
  if (curwin != old_curwin || curwin->w_cursor.lnum != old_cursor.lnum
      || curwin->w_cursor.col != old_cursor.col) {
    count |= CURSOR_MOVED;              // Cursor has moved
  }

  if (mouse_char == '+') {
    count |= MOUSE_FOLD_OPEN;
  } else if (mouse_char != ' ') {
    count |= MOUSE_FOLD_CLOSE;
  }

  return count;
}

// Compute the position in the buffer line from the posn on the screen in
// window "win".
// Returns true if the position is below the last line.
bool mouse_comp_pos(win_T *win, int *rowp, int *colp, linenr_T *lnump)
{
  int col = *colp;
  int row = *rowp;
  linenr_T lnum;
  bool retval = false;
  int off;
  int count;

  if (win->w_p_rl)
    col = win->w_width - 1 - col;

  lnum = win->w_topline;

  while (row > 0) {
    // Don't include filler lines in "count"
    if (win->w_p_diff
        && !hasFoldingWin(win, lnum, NULL, NULL, true, NULL)) {
      if (lnum == win->w_topline) {
        row -= win->w_topfill;
      } else {
        row -= diff_check_fill(win, lnum);
      }
      count = plines_win_nofill(win, lnum, true);
    } else {
      count = plines_win(win, lnum, true);
    }

    if (count > row) {
      break;            // Position is in this buffer line.
    }

    (void)hasFoldingWin(win, lnum, NULL, &lnum, true, NULL);

    if (lnum == win->w_buffer->b_ml.ml_line_count) {
      retval = true;
      break;                    // past end of file
    }
    row -= count;
    ++lnum;
  }

  if (!retval) {
    // Compute the column without wrapping.
    off = win_col_off(win) - win_col_off2(win);
    if (col < off)
      col = off;
    col += row * (win->w_width - off);
    // add skip column (for long wrapping line)
    col += win->w_skipcol;
  }

  if (!win->w_p_wrap) {
    col += win->w_leftcol;
  }

  // skip line number and fold column in front of the line
  col -= win_col_off(win);
  if (col < 0) {
    col = 0;
  }

  *colp = col;
  *rowp = row;
  *lnump = lnum;
  return retval;
}

// Find the window at screen position "*rowp" and "*colp".  The positions are
// updated to become relative to the top-left of the window.
win_T *mouse_find_win(int *rowp, int *colp)
{
  frame_T     *fp;

  fp = topframe;
  *rowp -= firstwin->w_winrow;
  for (;; ) {
    if (fp->fr_layout == FR_LEAF)
      break;
    if (fp->fr_layout == FR_ROW) {
      for (fp = fp->fr_child; fp->fr_next != NULL; fp = fp->fr_next) {
        if (*colp < fp->fr_width)
          break;
        *colp -= fp->fr_width;
      }
    } else {  // fr_layout == FR_COL
      for (fp = fp->fr_child; fp->fr_next != NULL; fp = fp->fr_next) {
        if (*rowp < fp->fr_height)
          break;
        *rowp -= fp->fr_height;
      }
    }
  }
  return fp->fr_win;
}

/*
 * setmouse() - switch mouse on/off depending on current mode and 'mouse'
 */
void setmouse(void)
{
  int checkfor;


  /* be quick when mouse is off */
  if (*p_mouse == NUL)
    return;

  /* don't switch mouse on when not in raw mode (Ex mode) */
  if (cur_tmode != TMODE_RAW) {
    mch_setmouse(false);
    return;
  }

  if (VIsual_active)
    checkfor = MOUSE_VISUAL;
  else if (State == HITRETURN || State == ASKMORE || State == SETWSIZE)
    checkfor = MOUSE_RETURN;
  else if (State & INSERT)
    checkfor = MOUSE_INSERT;
  else if (State & CMDLINE)
    checkfor = MOUSE_COMMAND;
  else if (State == CONFIRM || State == EXTERNCMD)
    checkfor = ' ';     /* don't use mouse for ":confirm" or ":!cmd" */
  else
    checkfor = MOUSE_NORMAL;        /* assume normal mode */

  if (mouse_has(checkfor))
    mch_setmouse(true);
  else
    mch_setmouse(false);
}

/*
 * Return true if
 * - "c" is in 'mouse', or
 * - 'a' is in 'mouse' and "c" is in MOUSE_A, or
 * - the current buffer is a help file and 'h' is in 'mouse' and we are in a
 *   normal editing mode (not at hit-return message).
 */
int mouse_has(int c)
{
  for (char_u *p = p_mouse; *p; ++p)
    switch (*p) {
    case 'a': if (vim_strchr((char_u *)MOUSE_A, c) != NULL)
        return true;
      break;
    case MOUSE_HELP: if (c != MOUSE_RETURN && curbuf->b_help)
        return true;
      break;
    default: if (c == *p) return true; break;
    }
  return false;
}
