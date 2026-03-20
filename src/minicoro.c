/* minicoro.c — single instantiation of the minicoro header-only library.
 *
 * MINICORO_IMPL must be defined in exactly one translation unit before
 * including minicoro.h.  All other files include minicoro.h without this
 * define and get only the declarations.
 */
#define MINICORO_IMPL
#include "minicoro.h"

/* ---------------------------------------------------------------------------
 * mco_get_suspended_sp — return the saved stack pointer of a suspended fiber.
 *
 * Provides access to the fiber's actual CPU-saved SP (stored in the
 * platform-specific context buffer) without exposing _mco_context to other
 * translation units.  Only valid when mco_status(co) == MCO_SUSPENDED.
 *
 * On x86_64 (ASM): returns _mco_context->ctx.rsp
 * On aarch64 (ASM): returns _mco_context->ctx.sp
 * On other backends (ucontext, fibers, Wasm): returns NULL — caller must
 * fall back to scanning the full stack.
 * --------------------------------------------------------------------------- */
void* mco_get_suspended_sp(mco_coro* co)
{
    if (co == NULL || co->context == NULL)
        return NULL;
#if defined(MCO_USE_ASM)
    _mco_context* ctx = (_mco_context*)co->context;
#  if defined(__x86_64__) || defined(_M_X64)
    return ctx->ctx.rsp;
#  elif defined(__aarch64__) || defined(__arm64__)
    return ctx->ctx.sp;
#  else
    return NULL;
#  endif
#else
    return NULL;
#endif
}
