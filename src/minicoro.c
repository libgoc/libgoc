/* minicoro.c — single instantiation of the minicoro header-only library.
 *
 * MINICORO_IMPL must be defined in exactly one translation unit before
 * including minicoro.h.  All other files include minicoro.h without this
 * define and get only the declarations.
 */
#define MINICORO_IMPL
#include "minicoro.h"
