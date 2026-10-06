/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#ifndef LG_WINDOWS_CLIENT_HCS_SYSTEMS_H
#define LG_WINDOWS_CLIENT_HCS_SYSTEMS_H

/* A summary of what HcsEnumerateComputeSystems returned, for the HCS probe's
 * report. The report goes to whoever the person who ran the probe sends it
 * to, and the HCS lists every virtual machine of the PC, with the ids, and for
 * some the names, that identify them. The summary has what the probe needs to
 * see and no more: how many systems there are, and the type, the owner and
 * the state of each.
 *
 *   {"systems":[{"system_type":"VirtualMachine","owner":"VMMS",
 *    "state":"Running"}],"count":1,"unread":0}
 *
 * unread counts what was not a JSON object that could be read, such as a
 * system the document cut short. doc is the document that the HCS returned, or
 * NULL. Returns text that the caller frees, or NULL if memory ran out. */
char * hcsSystemsSummary(const char * doc);

#endif
