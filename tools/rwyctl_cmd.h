/* tools/rwyctl_cmd.h: command execution dispatch shared between rwyctl and rwy-fuse. */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RWYCTL_CMD_H
#define RWYCTL_CMD_H

int has_arg(int argc, char **argv, const char *a);
int command_writes(int argc, char **argv, int cmd);
int rwyctl_exec_command(int argc, char **argv);

#endif
