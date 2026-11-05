/* SPDX-License-Identifier: Apache-2.0 */
#ifndef TESTDIR_H
#define TESTDIR_H

/*
 * Path of a scratch file in a per-run directory under /var/tmp (rwy-test.*).
 * The directory is made on first use and removed when the process exits or is stopped by a signal.
 * Directories of dead processes are removed on first use.
 * The same name gives the same pointer; it stays valid until exit.
 */
const char *tpath(const char *name);

/* The mkstore program: $MKSTORE, or build/mkstore. */
const char *mkstore_path(void);

#endif
