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

/*
 * Checks which named sections the client's ivshmemOpenDev() takes, by making
 * sections in this process with different security and opening them as the
 * client does. The client trusts what is in a section with the guest's screen
 * and the user's keystrokes, so it takes only one that this user made, and
 * that only this user, the system and the VMs can open.
 */

#include "common/debug.h"
#include "common/ivshmem.h"

#include <windows.h>
#include <sddl.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTION_SIZE (1024u * 1024u)

struct Case
{
  const char * name;
  const char * dacl;      /* after the user's and the system's access */
  const char * sacl;      /* the integrity label, if any */
  bool         accepted;
};

static const struct Case cases[] =
{
  { "this user and the system"             , ""               , ""                  , true  },
  { "with a label of medium integrity"     , ""               , "S:(ML;;NW;;;ME)"   , true  },
  { "with a label of low integrity"        , ""               , "S:(ML;;NW;;;LW)"   , false },
  { "with a label of untrusted integrity"  , ""               , "S:(ML;;NW;;;S-1-16-0)", false },
  { "every account may read and write it"  , "(A;;GRGW;;;WD)" , ""                  , false },
  { "authenticated users may write it"     , "(A;;GW;;;AU)"   , ""                  , false },
  { "authenticated users may map it"       , "(A;;0x2;;;AU)"  , ""                  , false },
};

static char * userSid(void)
{
  HANDLE token = NULL;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    return NULL;

  DWORD size = 0;
  GetTokenInformation(token, TokenUser, NULL, 0, &size);
  TOKEN_USER * user = size ? malloc(size) : NULL;
  char * sid = NULL;
  if (user && GetTokenInformation(token, TokenUser, user, size, &size))
    ConvertSidToStringSidA(user->User.Sid, &sid);
  free(user);
  CloseHandle(token);
  return sid;
}

int main(void)
{
  debug_init();

  char * sid = userSid();
  if (!sid)
  {
    printf("cannot read this user's SID: error %lu\n", GetLastError());
    return 2;
  }

  int failures = 0;
  for(unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
  {
    char sddl[512];
    snprintf(sddl, sizeof(sddl), "O:%sD:P(A;;GA;;;SY)(A;;GA;;;%s)%s%s", sid,
        sid, cases[i].dacl, cases[i].sacl);

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl,
          SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL))
    {
      printf("FAIL %s: the security %s is not valid: error %lu\n",
          cases[i].name, sddl, GetLastError());
      ++failures;
      continue;
    }

    char name[96];
    snprintf(name, sizeof(name), "Local\\lg-section-test-%lu-%u",
        GetCurrentProcessId(), i);
    HANDLE section = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa,
        PAGE_READWRITE | SEC_COMMIT, 0, SECTION_SIZE, name);
    const DWORD error = GetLastError();
    LocalFree(sa.lpSecurityDescriptor);
    if (!section)
    {
      printf("FAIL %s: cannot make the section: error %lu\n", cases[i].name,
          error);
      ++failures;
      continue;
    }

    struct IVSHMEM dev = { 0 };
    const bool accepted = ivshmemOpenDev(&dev, name);
    if (accepted != cases[i].accepted)
    {
      printf("FAIL %s: the client %s the section, and should have %s it\n",
          cases[i].name, accepted ? "took" : "refused",
          cases[i].accepted ? "taken" : "refused");
      ++failures;
    }
    else
      printf("ok   %s: the client %s the section\n", cases[i].name,
          accepted ? "takes" : "refuses");

    if (accepted)
    {
      if (!dev.mem || dev.size != SECTION_SIZE)
      {
        printf("FAIL %s: the mapping is %u bytes, and not %u\n",
            cases[i].name, dev.size, SECTION_SIZE);
        ++failures;
      }

      // a section is given back by ivshmemClose, and ivshmemFree after it
      // has nothing left to do
      ivshmemClose(&dev);
      ivshmemFree(&dev);
    }
    CloseHandle(section);
  }

  LocalFree(sid);
  if (failures)
    printf("%d failed\n", failures);
  else
    printf("all passed\n");
  return failures ? 1 : 0;
}
