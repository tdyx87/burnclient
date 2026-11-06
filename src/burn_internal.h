#ifndef BURN_INTERNAL_H
#define BURN_INTERNAL_H

/* ---------------------------------------------------------------------------
 * Internal helpers shared between the C API layer and the platform backends.
 *
 * This header is NOT installed and is NEVER part of the public ABI. Some of
 * these functions take C++ types, so they must not be declared extern "C".
 * Platform .cpp files include it instead of repeating ad-hoc declarations.
 * ------------------------------------------------------------------------- */

#include "burning_utility.h"
#include <string>

#if defined(__cplusplus)

/* Set the calling thread's last-error string (thread_local). */
void SetBurnLastError(const std::string& error);

/* Report progress. Safe to call from ANY thread: the active job is looked up
 * under a mutex. Caller callbacks are wrapped in try/catch(...). */
void NotifyProgressBurn(int percent, const char* status);

/* Progress for legacy synchronous calls: only the global callback, never
 * attributed to a concurrently running job. */
void NotifyLegacyProgress(int percent, const char* status);

/* True if the currently active job has been asked to cancel. Platform loops
 * poll this; the cancel hook below gives immediate notification. */
bool IsBurnCancelled();

/* A backend may register a short, non-blocking hook that is invoked by
 * Burn_Cancel() to interrupt a blocking native call (IMAPI2 CancelWrite,
 * libburn burn_drive_cancel, kill(child, SIGTERM), ...).
 * The hook is called outside the job mutex, but ClearBurnCancelHook() waits
 * for any in-flight hook to return, so the context stays valid. */
typedef void (*BurnCancelHook)(void* context);
void SetBurnCancelHook(BurnCancelHook hook, void* context);
void ClearBurnCancelHook(BurnCancelHook hook, void* context);

#endif /* __cplusplus */
#endif /* BURN_INTERNAL_H */
