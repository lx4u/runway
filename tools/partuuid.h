/* partuuid: find the partition of a GPT partition UUID, with or without udev. */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef PARTUUID_H
#define PARTUUID_H

/* The partition (sysfs name, "sda1") and disk ("sda") of a partition UUID text. root prefixes /dev and /sys
 * (a test tree, "" for the system). part and disk hold 64 bytes. Returns 1 when found, else 0. */
int partuuid_find(const char *root, const char *uuid, char *part, char *disk);

/* Whether a partition (sysfs name, with its disk) has the GPT type GUID text `type`: 1 yes, 0 no, -1 unknown
 * (the disk has no readable GPT). */
int partuuid_has_type(const char *root, const char *disk, const char *part, const char *type);

#endif
