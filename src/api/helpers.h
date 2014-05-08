#ifndef NEOVIM_API_HELPERS_H
#define NEOVIM_API_HELPERS_H

#include <stdbool.h>

#include "api/defs.h"

#define set_api_error(message, err)                \
  do {                                             \
    strncpy(err->msg, message, sizeof(err->msg));  \
    err->set = true;                               \
  } while (0)

/// Start block that may cause vimscript exceptions
void try_start(void);

/// End try block, set the error message if any and return true if an error
/// occurred.
///
/// @param err Pointer to the stack-allocated error object
/// @return true if an error occurred
bool try_end(Error *err);

#endif /* NEOVIM_API_HELPERS_H */

