/*
 * Build-configuration guards.
 *
 * CLAUDE.md 6: "No secrets in the repository. Keys are provisioned at runtime.
 * The development-key compile flag must fail a release build."  And 2.1: "A
 * development key behind a compile flag is permitted for bench work in phases
 * 2-3; that flag must fail the build in release configuration."
 *
 * There are two halves to this and both are needed:
 *
 *   scripts/release_guard.py  refuses to start a release build with MAXL_DEV_KEY
 *                             set in the environment
 *   this header               refuses to compile one, so that a build invoked
 *                             around the pre-hook still fails
 *
 * One check is a policy. Two checks in different layers is a guard.
 *
 * MAXL_BUILD: 1 = debug, 2 = release (platformio.ini).
 */

#ifndef MAXL_BUILD_GUARD_H
#define MAXL_BUILD_GUARD_H

#ifndef MAXL_BUILD
#error "MAXL_BUILD is not defined -- build through platformio.ini, not by hand."
#endif

#define MAXL_BUILD_DEBUG 1
#define MAXL_BUILD_RELEASE 2

#if MAXL_BUILD == MAXL_BUILD_RELEASE

#ifdef MAXL_DEV_KEY
#error "MAXL_DEV_KEY is set in a release build. The development network key is for bench work only (CLAUDE.md 2.1). Unset MAXL_DEV_KEY and rebuild."
#endif

#ifdef MAXL_BENCH
#error "MAXL_BENCH is set in a release build. The bench seam (Node::benchFrame/benchTransmit, gates 2.2/2.3) is for bring-up images only."
#endif

#if defined(MAXL_LOG_LEVEL) && MAXL_LOG_LEVEL > 0
#error "Serial debug output must be compiled out in release builds (CLAUDE.md 6). MAXL_LOG_LEVEL must be 0."
#endif

#endif // release

/*
 * The development key never appears in a file. It comes from the environment,
 * gets stringified by scripts/release_guard.py, and lives only in the object
 * code of a debug build -- so `git grep` for a key can never find one, because
 * there has never been one to find.
 */
#ifdef MAXL_DEV_KEY
#define MAXL_HAVE_DEV_KEY 1
#else
#define MAXL_HAVE_DEV_KEY 0
#endif

#endif // MAXL_BUILD_GUARD_H
