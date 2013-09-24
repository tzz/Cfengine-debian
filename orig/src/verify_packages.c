/* 
   Copyright (C) Cfengine AS

   This file is part of Cfengine 3 - written and maintained by Cfengine AS.
 
   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
 
  You should have received a copy of the GNU General Public License  
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of Cfengine, the applicable Commerical Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.

*/

/*****************************************************************************/
/*                                                                           */
/* File: verify_packages.c                                                   */
/*                                                                           */
/*****************************************************************************/

#include "cf3.defs.h"
#include "cf3.extern.h"

static void VerifyPromisedPatch(struct Attributes a,struct Promise *pp);
static int ExecuteSchedule(struct CfPackageManager *schedule,enum package_actions action);
static int ExecutePatch(struct CfPackageManager *schedule,enum package_actions action);
static int PackageSanityCheck(struct Attributes a,struct Promise *pp);
static int VerifyInstalledPackages(struct CfPackageManager **alllists,struct Attributes a,struct Promise *pp);
static void VerifyPromisedPackage(struct Attributes a,struct Promise *pp);
static void DeletePackageItems(struct CfPackageItem *pi);
static int PackageMatch(char *n,char *v,char *a,struct Attributes attr,struct Promise *pp);
static int PatchMatch(char *n,char *v,char *a,struct Attributes attr,struct Promise *pp);
static int ComparePackages(char *n,char *v,char *a,struct CfPackageItem *pi,enum version_cmp cmp);
static void ParsePackageVersion(char *version,struct Rlist **num,struct Rlist **sep);
static void SchedulePackageOp(char *name,char *version,char *arch,int installed,int matched,int novers,struct Attributes a,struct Promise *pp);
static char *PrefixLocalRepository(struct Rlist *repositories,char *package);
static int FindLargestVersionAvail(char *matchName, char *matchVers, char *refAnyVer, char *ver, enum version_cmp package_select, struct Rlist *repositories);
static int VersionCmp(char *vs1, char *vs2);
static int IsNewerThanInstalled(char *n,char *v,char *a, char *instV, char *instA, struct Attributes attr);
static int PackageInItemList(struct CfPackageItem *list,char *name,char *version,char *arch);
static int ExecPackageCommandGeneric(char *command,int verify,int setCmdClasses,struct Attributes a,struct Promise *pp);
static int PrependPatchItem(struct CfPackageItem **list,char *item,struct CfPackageItem *chklist,struct Attributes a,struct Promise *pp);
static int PrependMultiLinePackageItem(struct CfPackageItem **list,char *item,int reset,struct Attributes a,struct Promise *pp);
static int ExecPackageCommand(char *command,int verify,int setCmdClasses,struct Attributes a,struct Promise *pp);
static void ReportSoftware(struct CfPackageManager *list);
static const char *GetSoftwareCacheFilename(char *buffer);
static void InvalidateSoftwareCache(void);
static void ExecutePackageSchedule(struct CfPackageManager *schedule);
static void DeletePackageManagers(struct CfPackageManager *newlist);
static struct CfPackageManager *NewPackageManager(struct CfPackageManager **lists,char *mgr,enum package_actions pa,enum action_policy x);
static struct CfPackageItem *GetCachedPackageList(struct CfPackageManager *manager,struct Attributes a,struct Promise *pp);

/*****************************************************************************/

struct CfPackageManager *PACKAGE_SCHEDULE = NULL;
struct CfPackageManager *INSTALLED_PACKAGE_LISTS = NULL;

/*****************************************************************************/

void VerifyPackagesPromise(struct Promise *pp)

{ struct Attributes a = {{0}},al = {{0}};
  struct CfLock thislock;
  char lockname[CF_BUFSIZE];

a = GetPackageAttributes(pp);

#ifdef MINGW

a.packages.package_list_command = "/Windows_Installer";

#endif

if (!PackageSanityCheck(a,pp))
   {
   return;
   }

PromiseBanner(pp);

// Now verify the package itself

snprintf(lockname,CF_BUFSIZE-1,"package-%s-%s",pp->promiser,a.packages.package_list_command);
 
thislock = AcquireLock(lockname,VUQNAME,CFSTARTTIME,a,pp,false);

if (thislock.lock == NULL)
   {
   return;
   }

// Start by reseting the root directory in case yum tries to glob regexs(!)

chdir("/");

if (!VerifyInstalledPackages(&INSTALLED_PACKAGE_LISTS,a,pp))
   {
   cfPS(cf_error,CF_FAIL,"",pp,a," !! Unable to obtain a list of installed packages - aborting");
   return;
   }

switch (a.packages.package_policy)
   {
   case cfa_patch:
       VerifyPromisedPatch(a,pp);
       break;
       
   default:
       VerifyPromisedPackage(a,pp);
       break;
   }

YieldCurrentLock(thislock);
}

/*****************************************************************************/

static int PackageSanityCheck(struct Attributes a,struct Promise *pp)

{
#ifndef MINGW
if (a.packages.package_list_version_regex == NULL)
   {
   cfPS(cf_error,CF_FAIL,"",pp,a," !! You must supply a method for determining the version of existing packages");
   return false;
   }

if (a.packages.package_list_name_regex == NULL)
   {
   cfPS(cf_error,CF_FAIL,"",pp,a," !! You must supply a method for determining the name of existing packages");
   return false;
   }
#endif  /* NOT MINGW */

if (a.packages.package_list_command == NULL && a.packages.package_file_repositories == NULL)
   {
   cfPS(cf_error,CF_FAIL,"",pp,a," !! You must supply a method for determining the list of existing packages (a command or repository list)");
   return false;
   }

if (a.packages.package_file_repositories)
   {
   struct Rlist *rp;

   for (rp = a.packages.package_file_repositories; rp != NULL; rp = rp->next)
      {
      if (strlen(rp->item) > CF_MAXVARSIZE-1)
         {
         cfPS(cf_error,CF_FAIL,"",pp,a," !! The repository path \"%s\" is too long",rp->item);
         return false;         
         }
      }
   }

if (a.packages.package_name_regex||a.packages.package_version_regex||a.packages.package_arch_regex)
   {
   if (a.packages.package_name_regex && a.packages.package_version_regex && a.packages.package_arch_regex)
      {
      if(a.packages.package_version || a.packages.package_architectures)
         {
         cfPS(cf_error,CF_FAIL,"",pp,a," !! You must either supply all regexs for (name,version,arch) xor a separate version number and architecture");
         return false;            
         }
      }
   else
      {
      if (a.packages.package_version && a.packages.package_architectures)
         {
         cfPS(cf_error,CF_FAIL,"",pp,a," !! You must either supply all regexs for (name,version,arch) xor a separate version number and architecture");
         return false;
         }
       }
   }

if (a.packages.package_add_command == NULL || a.packages.package_delete_command == NULL)
   {
   cfPS(cf_verbose,CF_FAIL,"",pp,a,"!! Package add/delete command undefined");
   return false;
   }

if (!a.packages.package_installed_regex)
   {
   cfPS(cf_verbose,CF_FAIL,"",pp,a,"!! Package installed regex undefined");
   return false;
   }

if(a.packages.package_policy == cfa_verifypack)
  {
  if(!a.packages.package_verify_command)
    {
    cfPS(cf_verbose,CF_FAIL,"",pp,a,"!! Package verify policy is used, but no package_verify_command is defined");
    return false;
    }
  else if((a.packages.package_noverify_returncode == CF_NOINT) && (a.packages.package_noverify_regex == NULL))
    {
    cfPS(cf_verbose,CF_FAIL,"",pp,a,"!! Package verify policy is used, but no definition of verification failiure is set (package_noverify_returncode or packages.package_noverify_regex)");
    return false;    
    }
  }

if((a.packages.package_noverify_returncode != CF_NOINT) && a.packages.package_noverify_regex)
  {
  cfPS(cf_verbose,CF_FAIL,"",pp,a,"!! Both package_noverify_returncode and package_noverify_regex are defined, pick one of them");
  return false;    
  }

return true;
}

/*****************************************************************************/

void ExecuteScheduledPackages(void)
{
if (PACKAGE_SCHEDULE)
   {
   ExecutePackageSchedule(PACKAGE_SCHEDULE);
   }
}

void CleanScheduledPackages(void)
{
DeletePackageManagers(PACKAGE_SCHEDULE);
PACKAGE_SCHEDULE = NULL;
DeletePackageManagers(INSTALLED_PACKAGE_LISTS);
INSTALLED_PACKAGE_LISTS = NULL;
}

/*****************************************************************************/

static void ExecutePackageSchedule(struct CfPackageManager *schedule)

{
CfOut(cf_verbose,""," >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\n");
CfOut(cf_verbose,"","   Offering these package-promise suggestions to the managers\n");
CfOut(cf_verbose,""," >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\n");
 
 /* Normal ordering */

CfOut(cf_verbose,""," -> Deletion schedule...\n");

if (!ExecuteSchedule(schedule,cfa_deletepack))
   {
   CfOut(cf_error,"","Aborting package schedule");
   return;
   }

CfOut(cf_verbose,""," -> Addition schedule...\n");

if (!ExecuteSchedule(schedule,cfa_addpack))
   {
   return;
   }

CfOut(cf_verbose,""," -> Update schedule...\n");

if (!ExecuteSchedule(schedule,cfa_update))
   {
   return;
   }            

CfOut(cf_verbose,""," -> Patch schedule...\n");

if (!ExecutePatch(schedule,cfa_patch))
   {
   return;
   }

CfOut(cf_verbose,""," -> Verify schedule...\n");

if (!ExecuteSchedule(schedule,cfa_verifypack))
   {
   return;
   }
}
          
/*****************************************************************************/
/* Level                                                                     */
/*****************************************************************************/

static int VerifyInstalledPackages(struct CfPackageManager **all_mgrs,struct Attributes a,struct Promise *pp)

{ struct CfPackageManager *manager = NewPackageManager(all_mgrs,a.packages.package_list_command,cfa_pa_none,cfa_no_ppolicy);
  const int reset = true, update = false;
  char vbuff[CF_BUFSIZE];
  FILE *prp;
  
if (manager == NULL)
   {
   CfOut(cf_error,""," !! Can't create a package manager envelope for \"%s\"",a.packages.package_list_command);
   return false;
   }

if (manager->pack_list != NULL)
   {
   CfOut(cf_verbose,""," ?? Already have a package list for this manager ");
   return true;
   }

manager->pack_list = GetCachedPackageList(manager,a,pp);

if (manager->pack_list != NULL)
   {
   CfOut(cf_verbose,""," ?? Already have a (cached) package list for this manager ");
   return true;
   }

#ifdef MINGW

CfOut(cf_verbose, "", " Using internal list command for Windows");

if (!NovaWin_GetInstalledPkgs(&(manager->pack_list),a,pp))
   {
   CfOut(cf_error, "", "!! Could not get list of installed packages");
   return false;
   }

#else

if (a.packages.package_list_command != NULL)
   {
   if(strcmp(a.packages.package_list_command,"/cf_internal_rpath_list") == 0)
      {
      CfOut(cf_verbose, "", " Using internal list command for rPath");
      
      if(!GetInstalledPkgsRpath(&(manager->pack_list),a,pp))
         {
         CfOut(cf_error, "", "!! Could not get list of installed packages");
         return false;
         }
      }
   else
      {
      if (a.packages.package_list_update_command != NULL)
         {
         ExecPackageCommand(a.packages.package_list_update_command,false,false,a,pp);
         }

      CfOut(cf_verbose,""," ???????????????????????????????????????????????????????????????\n");
      CfOut(cf_verbose,"","   Reading package list from %s\n",GetArg0(a.packages.package_list_command));
      CfOut(cf_verbose,""," ???????????????????????????????????????????????????????????????\n");
      
      if (!IsExecutable(GetArg0(a.packages.package_list_command)))
         {
         CfOut(cf_error,"","The proposed package list command \"%s\" was not executable",a.packages.package_list_command);
         return false;
         }
      
      if ((prp = cf_popen(a.packages.package_list_command,"r")) == NULL)
         {
         CfOut(cf_error,"cf_popen","Couldn't open the package list with command %s\n",a.packages.package_list_command);
         return false;
         }
      
      while (!feof(prp))
         {
         memset(vbuff,0,CF_BUFSIZE);
         CfReadLine(vbuff,CF_BUFSIZE,prp);
         CF_OCCUR++;
         
         if (a.packages.package_multiline_start)
            {
            if (FullTextMatch(a.packages.package_multiline_start,vbuff))
               {
               PrependMultiLinePackageItem(&(manager->pack_list),vbuff,reset,a,pp);
               }
            else
               {
               PrependMultiLinePackageItem(&(manager->pack_list),vbuff,update,a,pp);
               }
            }
         else
            {
            if (!FullTextMatch(a.packages.package_installed_regex,vbuff))
               {
               continue;
               }
            
            if (!PrependListPackageItem(&(manager->pack_list),vbuff,a,pp))
               {
               continue;
               }
            }
         }
      
      cf_pclose(prp);
      }
   }

#endif  /* NOT MINGW */

ReportSoftware(INSTALLED_PACKAGE_LISTS);

/* Now get available updates */

if (a.packages.package_patch_list_command != NULL)
   {
   CfOut(cf_verbose,""," ???????????????????????????????????????????????????????????????\n");
   CfOut(cf_verbose,"","   Reading patches from %s\n",GetArg0(a.packages.package_patch_list_command));
   CfOut(cf_verbose,""," ???????????????????????????????????????????????????????????????\n");

   if (!IsExecutable(GetArg0(a.packages.package_patch_list_command)))
      {
      CfOut(cf_error,"","The proposed patch list command \"%s\" was not executable",a.packages.package_patch_list_command);
      return false;
      }
   
   if ((prp = cf_popen(a.packages.package_patch_list_command,"r")) == NULL)
      {
      CfOut(cf_error,"cf_popen","Couldn't open the patch list with command %s\n",a.packages.package_patch_list_command);
      return false;
      }
   
   while (!feof(prp))
      {
      memset(vbuff,0,CF_BUFSIZE);
      CfReadLine(vbuff,CF_BUFSIZE,prp);   

      // assume patch_list_command lists available patches/updates by default
      if (a.packages.package_patch_installed_regex == NULL || !FullTextMatch(a.packages.package_patch_installed_regex,vbuff))
         {
         PrependPatchItem(&(manager->patch_avail),vbuff,manager->patch_list,a,pp);
         continue;
         }

      if (!PrependPatchItem(&(manager->patch_list),vbuff,manager->patch_list,a,pp))
         {
         continue;
         }
      }
   
   cf_pclose(prp);
   }

ReportPatches(INSTALLED_PACKAGE_LISTS);

CfOut(cf_verbose,""," ???????????????????????????????????????????????????????????????\n");
CfOut(cf_verbose,"","  Done checking packages and patches \n");
CfOut(cf_verbose,""," ???????????????????????????????????????????????????????????????\n");

return true;
}

/*****************************************************************************/
static int VersionCheckSchedulePackage(struct Attributes a, struct Promise *pp, int matches, int installed)

{
/* The meaning of matches and installed depends on the package policy */
enum package_actions policy = a.packages.package_policy;

switch(policy)
   {
   case cfa_deletepack:
      if (matches && installed)
         {
         return true;
         }
      break;

   case cfa_reinstall:
      if (matches && installed)
         {
         return true;
         }
      else
         {
	 cfPS(cf_verbose,CF_NOP,"",pp,a," -> Package (%s) already installed and matches criteria\n",pp->promiser);
	 }
      break;

   default:
      if (!installed || !matches)
         {
         return true;
	 }
      else
         {
	 cfPS(cf_verbose,CF_NOP,"",pp,a," -> Package (%s) already installed and matches criteria\n",pp->promiser);
	 }
      break;
   }

return false;
}

/*****************************************************************************/

static void VerifyPromisedPackage(struct Attributes a,struct Promise *pp)

{ char version[CF_MAXVARSIZE];
  char name[CF_MAXVARSIZE];
  char arch[CF_MAXVARSIZE];
  char *package = pp->promiser;
  int matches = 0, installed = 0, no_version = false;
  struct Rlist *rp;

if (a.packages.package_version) 
   {
   /* The version is specified separately */
   CfOut(cf_verbose,""," -> Package version specified explicitly in promise body");

   if (a.packages.package_architectures == NULL)
      {
      strncpy(name,pp->promiser,CF_MAXVARSIZE-1);
      strncpy(version,a.packages.package_version,CF_MAXVARSIZE-1);
      strncpy(arch,"*",CF_MAXVARSIZE-1);
      installed = PackageMatch(name,"*",arch,a,pp);
      matches = PackageMatch(name,version,arch,a,pp);

      if (VersionCheckSchedulePackage(a, pp, matches, installed))
         {
         SchedulePackageOp(name,version,arch,installed,matches,no_version,a,pp);
         }
      }
   else
      {
      for (rp = a.packages.package_architectures; rp != NULL; rp=rp->next)
         {
         CfOut(cf_verbose,""," ... trying listed arch %s\n",rp->item);
         strncpy(name,pp->promiser,CF_MAXVARSIZE-1);
         strncpy(version,a.packages.package_version,CF_MAXVARSIZE-1);
         strncpy(arch,rp->item,CF_MAXVARSIZE-1);

         installed = PackageMatch(name,"*",arch,a,pp);
         matches = PackageMatch(name,version,arch,a,pp);

         if (VersionCheckSchedulePackage(a, pp, matches, installed))
            {
            SchedulePackageOp(name,version,arch,installed,matches,no_version,a,pp);
            }
	 }
      }
   }
else if (a.packages.package_version_regex)
   {
   /* The name, version and arch are to be extracted from the promiser */
   CfOut(cf_verbose,""," -> Package version specified implicitly in promiser's name");
      
   strlcpy(version,ExtractFirstReference(a.packages.package_version_regex,package),CF_MAXVARSIZE);
   strlcpy(name,ExtractFirstReference(a.packages.package_name_regex,package),CF_MAXVARSIZE);
   strlcpy(arch,ExtractFirstReference(a.packages.package_arch_regex,package),CF_MAXVARSIZE);

   if (strlen(arch) == 0)
      {
      strncpy(arch,"*",CF_MAXVARSIZE-1);
      }

   if (strcmp(arch,"CF_NOMATCH") == 0)  // no match on arch regex, use any arch
      {
      strcpy(arch,"*");
      }
   
   installed = PackageMatch(name,"*",arch,a,pp);
   matches = PackageMatch(name,version,arch,a,pp);

   if (VersionCheckSchedulePackage(a, pp, matches, installed))
      {
      SchedulePackageOp(name,version,arch,installed,matches,no_version,a,pp);
      }
   }
else
   {
   no_version = true;
   CfOut(cf_verbose,""," -> Package version was not specified");
         
   if (a.packages.package_architectures == NULL)
      {
      strncpy(name,pp->promiser,CF_MAXVARSIZE-1);
      strncpy(version,"*",CF_MAXVARSIZE-1);
      strncpy(arch,"*",CF_MAXVARSIZE-1);
      installed = PackageMatch(name,"*",arch,a,pp);
      matches = PackageMatch(name,version,arch,a,pp);

      SchedulePackageOp(name,version,arch,installed,matches,no_version,a,pp);
      }
   else
     {
     for (rp = a.packages.package_architectures; rp != NULL; rp=rp->next)
       {
       CfOut(cf_verbose,""," ... trying listed arch %s\n",rp->item);
       strncpy(name,pp->promiser,CF_MAXVARSIZE-1);
       strncpy(version,"*",CF_MAXVARSIZE-1);
       strncpy(arch,rp->item,CF_MAXVARSIZE-1);
       installed = PackageMatch(name,"*",arch,a,pp);
       matches = PackageMatch(name,version,arch,a,pp);

       SchedulePackageOp(name,version,arch,installed,matches,no_version,a,pp);
       }
     }
   }
}

/*****************************************************************************/

static void VerifyPromisedPatch(struct Attributes a,struct Promise *pp)

{ char version[CF_MAXVARSIZE];
  char name[CF_MAXVARSIZE];
  char arch[CF_MAXVARSIZE];
  char *package = pp->promiser;
  int matches = 0, installed = 0, no_version = false;
  struct Rlist *rp;
  
if (a.packages.package_version) 
   {
   /* The version is specified separately */

   for (rp = a.packages.package_architectures; rp != NULL; rp=rp->next)
      {
      strncpy(name,pp->promiser,CF_MAXVARSIZE-1);
      strncpy(version,a.packages.package_version,CF_MAXVARSIZE-1);
      strncpy(arch,rp->item,CF_MAXVARSIZE-1);
      installed += PatchMatch(name,"*","*",a,pp);
      matches += PatchMatch(name,version,arch,a,pp);
      }

   if (a.packages.package_architectures == NULL)
      {
      strncpy(name,pp->promiser,CF_MAXVARSIZE-1);
      strncpy(version,a.packages.package_version,CF_MAXVARSIZE-1);
      strncpy(arch,"*",CF_MAXVARSIZE-1);
      installed = PatchMatch(name,"*","*",a,pp);
      matches = PatchMatch(name,version,arch,a,pp);
      }
   }
else if (a.packages.package_version_regex)
   {
   /* The name, version and arch are to be extracted from the promiser */
   strncpy(version,ExtractFirstReference(a.packages.package_version_regex,package),CF_MAXVARSIZE-1);
   strncpy(name,ExtractFirstReference(a.packages.package_name_regex,package),CF_MAXVARSIZE-1);
   strncpy(arch,ExtractFirstReference(a.packages.package_arch_regex,package),CF_MAXVARSIZE-1);
   installed = PatchMatch(name,"*","*",a,pp);
   matches = PatchMatch(name,version,arch,a,pp);
   }
else
   {
   no_version = true;
   
   for (rp = a.packages.package_architectures; rp != NULL; rp=rp->next)
      {
      strncpy(name,pp->promiser,CF_MAXVARSIZE-1);
      strncpy(version,"*",CF_MAXVARSIZE-1);
      strncpy(arch,rp->item,CF_MAXVARSIZE-1);
      installed += PatchMatch(name,"*","*",a,pp);
      matches += PatchMatch(name,version,arch,a,pp);
      }
   
   if (a.packages.package_architectures == NULL)
      {
      strncpy(name,pp->promiser,CF_MAXVARSIZE-1);
      strncpy(version,"*",CF_MAXVARSIZE-1);
      strncpy(arch,"*",CF_MAXVARSIZE-1);
      installed = PatchMatch(name,"*","*",a,pp);
      matches = PatchMatch(name,version,arch,a,pp);
      }
   }

CfOut(cf_verbose,""," -> %d patch(es) matching the name \"%s\" already installed\n",installed,name);
CfOut(cf_verbose,""," -> %d patch(es) match the promise body's criteria fully\n",matches,name);

SchedulePackageOp(name,version,arch,installed,matches,no_version,a,pp);
}

/*****************************************************************************/

static int ExecuteSchedule(struct CfPackageManager *schedule,enum package_actions action)

{ struct CfPackageItem *pi;
  struct CfPackageManager *pm;
  int size,estimated_size,retval = true,verify = false;
  char *command_string = NULL;
  struct Attributes a = {{0}};
  struct Promise *pp;
  int ok;       

for (pm = schedule; pm != NULL; pm = pm->next)
   {
   if (pm->action != action)
      {
      continue;
      }
   
   if (pm->pack_list == NULL)
      {
      continue;
      }

   estimated_size = 0;

   for (pi = pm->pack_list; pi != NULL; pi=pi->next)
      {   
      size = strlen(pi->name) + strlen("  ");
      
      switch (pm->policy)
         {
         case cfa_individual:             

             if (size > estimated_size)
                {
                estimated_size = size + CF_MAXVARSIZE;
                }             
             break;
             
         case cfa_bulk:

	     estimated_size += size + CF_MAXVARSIZE;
             break;

         default:
             break;
         }
      }

   pp = pm->pack_list->pp;
   a = GetPackageAttributes(pp);

   switch (action)
      {
      case cfa_addpack:

          if (a.packages.package_add_command == NULL)
             {
             cfPS(cf_verbose,CF_FAIL,"",pp,a,"Package package add command undefined");
             return false;
             }

          CfOut(cf_verbose,"","Execute scheduled package addition");

          if ((command_string = (malloc(estimated_size + strlen(a.packages.package_add_command) + 2))))
             {
             strcpy(command_string,a.packages.package_add_command);
             }
          break;

      case cfa_deletepack:

          if (a.packages.package_delete_command == NULL)
             {
             cfPS(cf_verbose,CF_FAIL,"",pp,a,"Package delete command undefined");
             return false;
             }

          CfOut(cf_verbose,"","Execute scheduled package deletion");

          if ((command_string = (malloc(estimated_size + strlen(a.packages.package_delete_command) + 2))))
             {
             strcpy(command_string,a.packages.package_delete_command);
             }
          break;
          
      case cfa_update:

          CfOut(cf_verbose,"","Execute scheduled package update");
                    
          if (a.packages.package_update_command == NULL)
             {
             cfPS(cf_verbose,CF_FAIL,"",pp,a,"Package update command undefined");
             return false;
             }

          if ((command_string = (malloc(estimated_size + strlen(a.packages.package_update_command) + 2))))
             {
             memset(command_string, 0, strlen(a.packages.package_update_command) + 2);
             strcpy(command_string,a.packages.package_update_command);
             }

          break;

      case cfa_verifypack:

          CfOut(cf_verbose,"","Execute scheduled package verification");
                    
          if (a.packages.package_verify_command == NULL)
             {
             cfPS(cf_verbose,CF_FAIL,"",pp,a,"Package verify command undefined");
             return false;
             }
          
          if ((command_string = (malloc(estimated_size + strlen(a.packages.package_verify_command) + 2))))
             {
             strcpy(command_string,a.packages.package_verify_command);
             }

          verify = true;
          break;
          
      default:
          cfPS(cf_verbose,CF_FAIL,"",pp,a,"Unknown action attempted");
          return false;
      }
      
   /* if the command ends with $ then we assume the package manager does not accept package names */
   
   if (*(command_string+strlen(command_string)-1) == '$')
      {
      *(command_string+strlen(command_string)-1) = '\0';
      CfOut(cf_verbose,"","Command does not allow arguments");
      if (ExecPackageCommand(command_string,verify,true,a,pp))
         {
	 CfOut(cf_verbose,"","Package schedule execution ok (outcome cannot be promised by cf-agent)");
         }
      else
         {
         CfOut(cf_error,"","!! Package schedule execution failed");
         }
      }
   else
      {
      strcat(command_string," ");
      
      CfOut(cf_verbose,"","Command prefix: %s\n",command_string);

      switch (pm->policy)
         {
         case cfa_individual:             
             
             for (pi = pm->pack_list; pi != NULL; pi=pi->next)
                {
                char *sp,*offset = command_string + strlen(command_string);

                if (a.packages.package_file_repositories && ( action == cfa_addpack || action == cfa_update ) )
                   {
                   if ((sp = PrefixLocalRepository(a.packages.package_file_repositories,pi->name)) != NULL)
                      {
                      strcat(offset,sp);
                      }
                   else
                      {
                      break;
                      }
                   }
		else
                   {
                   strcat(offset,pi->name);
                   }
                
                if (ExecPackageCommand(command_string,verify,true,a,pp))
                   {
                   CfOut(cf_verbose,"","Package schedule execution ok for %s (outcome cannot be promised by cf-agent)",pi->name);
                   }
                else
                   {
                   CfOut(cf_error,"","Package schedule execution failed for %s",pi->name);
                   }
                
                *offset = '\0';
                }
             
             break;
             
         case cfa_bulk:
             
             for (pi = pm->pack_list; pi != NULL; pi=pi->next)
                {
                if (pi->name)
                   {
		   char *sp, *offset = command_string + strlen(command_string);

                    if (a.packages.package_file_repositories && ( action == cfa_addpack || action == cfa_update ) )
                       {
    	               if ((sp = PrefixLocalRepository(a.packages.package_file_repositories,pi->name)) != NULL)
                          {
                          strcat(offset,sp);
                          }
                       else
                          {
                          break;
                          }
                       }
                    else
                       {
                       strcat(offset,pi->name);
    	               }

                   strcat(command_string," ");
                   }
                }

	     ok = ExecPackageCommand(command_string,verify,true,a,pp);
	     
	     for (pi = pm->pack_list; pi != NULL; pi=pi->next)
	       {
		 if (ok)
		   {
                   CfOut(cf_verbose,"","Bulk package schedule execution ok for %s (outcome cannot be promised by cf-agent)",pi->name);
		   }
		 else
                    {
                    CfOut(cf_error,"","Bulk package schedule execution failed somewhere - unknown outcome for %s",pi->name);
                    }
	       }
             
             break;
             
         default:
             break;
         }
      }
   }

if (command_string)
   {
   free(command_string);
   }

/* We have performed some operation on packages, our cache is invalid */
InvalidateSoftwareCache();

return retval;
}


/*****************************************************************************/

static int ExecutePatch(struct CfPackageManager *schedule,enum package_actions action)

{ struct CfPackageItem *pi;
  struct CfPackageManager *pm;
  int size,estimated_size,retval = true,verify = false;
  char *command_string = NULL;
  struct Attributes a = {{0}};
  struct Promise *pp;

for (pm = schedule; pm != NULL; pm = pm->next)
   {
   if (pm->action != action)
      {
      continue;
      }
   
   if (pm->patch_list == NULL)
      {
      continue;
      }

   estimated_size = 0;

   for (pi = pm->patch_list; pi != NULL; pi=pi->next)
      {   
      size = strlen(pi->name) + strlen("  ");
      
      switch (pm->policy)
         {
         case cfa_individual:             

             if (size > estimated_size)
                {
                estimated_size = size;
                }             
             break;
             
         case cfa_bulk:

             estimated_size += size;
             break;

         default:
             break;
         }
      }

   pp = pm->patch_list->pp;
   a = GetPackageAttributes(pp);

   switch (action)
      {
      case cfa_patch:

          CfOut(cf_verbose,"","Execute scheduled package patch");
          
          if (a.packages.package_patch_command == NULL)
             {
             cfPS(cf_verbose,CF_FAIL,"",pp,a,"Package patch command undefined");
             return false;
             }
          
          if ((command_string = (malloc(estimated_size + strlen(a.packages.package_patch_command) + 2))))
             {
             strcpy(command_string,a.packages.package_patch_command);
             }
          break;
          
      default:
          cfPS(cf_verbose,CF_FAIL,"",pp,a,"Unknown action attempted");
          return false;
      }

   /* if the command ends with $ then we assume the package manager does not accept package names */
   
   if (*(command_string+strlen(command_string)-1) == '$')
      {
      *(command_string+strlen(command_string)-1) = '\0';
      CfOut(cf_verbose,"","Command does not allow arguments");
      if (ExecPackageCommand(command_string,verify,true,a,pp))
         {
         CfOut(cf_verbose,"","Package patching seemed to succeed (outcome cannot be promised by cf-agent)");
         }
      else
         {
         CfOut(cf_error,"","Package patching failed");
         }
      }
   else
      {
      strcat(command_string," ");
      
      CfOut(cf_verbose,"","Command prefix: %s\n",command_string);

      switch (pm->policy)
         {
         int ok;
         
         case cfa_individual:             
             
             for (pi = pm->patch_list; pi != NULL; pi=pi->next)
                {
                char *offset = command_string + strlen(command_string);
                
                strcat(offset,pi->name);
                
                if (ExecPackageCommand(command_string,verify,true,a,pp))
                   {
                   CfOut(cf_verbose,"","Package schedule execution ok for %s (outcome cannot be promised by cf-agent)",pi->name);
                   }
                else
                   {
                   CfOut(cf_error,"","Package schedule execution failed for %s",pi->name);
                   }
                
                *offset = '\0';
                }
             
             break;
             
         case cfa_bulk:
             
             for (pi = pm->patch_list; pi != NULL; pi=pi->next)
                {
                if (pi->name)
                   {
                   strcat(command_string,pi->name);
                   strcat(command_string," ");
                   }
                }
             
             ok = ExecPackageCommand(command_string,verify,true,a,pp);
             
             for (pi = pm->patch_list; pi != NULL; pi=pi->next)
                {
                if (ok)
                   {
                   CfOut(cf_verbose,"","Bulk package schedule execution ok for %s (outcome cannot be promised by cf-agent)",pi->name);
                   }
                else
                   {
                   CfOut(cf_error,"","Bulk package schedule execution failed somewhere - unknown outcome for %s",pi->name);
                   }
                }
             
             break;
             
         default:
             break;
         }
      
      }
   }

if (command_string)
   {
   free(command_string);
   }

/* We have performed some operation on packages, our cache is invalid */
InvalidateSoftwareCache();

return retval;
}

/*****************************************************************************/
/* Level                                                                     */
/*****************************************************************************/

static struct CfPackageManager *NewPackageManager(struct CfPackageManager **lists,char *mgr,enum package_actions pa,enum action_policy policy)

{ struct CfPackageManager *np;

if (mgr == NULL || strlen(mgr) == 0)
   {
   CfOut(cf_error,""," !! Attempted to create a package manager with no name");
   return NULL;
   }

for (np = *lists; np != NULL; np=np->next)
   {
   if ((strcmp(np->manager,mgr) == 0) && (policy == np->policy))
      {
      return np;
      }
   }

if ((np = (struct CfPackageManager *)malloc(sizeof(struct CfPackageManager))) == NULL)
   {
   CfOut(cf_error,"malloc","Can't allocate new package\n");
   return NULL;
   }

np->manager = strdup(mgr);
np->action = pa;
np->policy = policy;
np->pack_list = NULL;
np->patch_list = NULL;
np->patch_avail = NULL;
np->next = *lists;
*lists = np;
return np;
}

/*****************************************************************************/

static void DeletePackageManagers(struct CfPackageManager *newlist)

{ struct CfPackageManager *np,*next;

for (np = newlist; np != NULL; np = next)
   {
   next = np->next;
   DeletePackageItems(np->pack_list);
   free((char *)np);
   }
}

/*****************************************************************************/

static struct CfPackageItem *GetCachedPackageList(struct CfPackageManager *manager,struct Attributes a,struct Promise *pp)

{ struct CfPackageItem *list = NULL;
  char name[CF_MAXVARSIZE],version[CF_MAXVARSIZE],arch[CF_MAXVARSIZE],mgr[CF_MAXVARSIZE],line[CF_BUFSIZE];
  char thismanager[CF_MAXVARSIZE];
  FILE *fin;
  time_t horizon = 24*60,now = time(NULL);
  struct stat sb;

GetSoftwareCacheFilename(name);

if (stat(name,&sb) == -1)
   {
   return NULL;
   }

if (a.packages.package_list_update_ifelapsed != CF_NOINT)
   {
   horizon = a.packages.package_list_update_ifelapsed;
   }
    
if (now - sb.st_mtime < horizon*60)
   {
   CfOut(cf_verbose,""," -> Cache file exists and is sufficiently fresh according to (package_list_update_ifelapsed)");
   }
else
   {
   CfOut(cf_verbose,""," -> Cache file exists, but it is out of date (package_list_update_ifelapsed)");
   return NULL;
   }

if ((fin = fopen(name,"r")) == NULL)
   {
   CfOut(cf_inform,"fopen","Cannot open the source log %s - you need to run a package discovery promise to create it in cf-agent",name);
   return NULL;
   }

/* Max 2016 entries - at least a week */

snprintf(thismanager,CF_MAXVARSIZE-1,"%s",ReadLastNode(GetArg0(manager->manager)));
             
while (!feof(fin))
   {
   line[0] = '\0';
   fgets(line,CF_BUFSIZE-1,fin);
   sscanf(line,"%250[^,],%250[^,],%250[^,],%250[^\n]",name,version,arch,mgr);

   if (strcmp(thismanager,mgr) == 0)
      {
      Debug("READPKG: %s\n",line);
      PrependPackageItem(&list,name,version,arch,a,pp);
      }
   }

fclose(fin);
return list;
}


/*****************************************************************************/

static int PrependMultiLinePackageItem(struct CfPackageItem **list,char *item,int reset,struct Attributes a,struct Promise *pp)

{ static char name[CF_MAXVARSIZE];
  static char arch[CF_MAXVARSIZE];
  static char version[CF_MAXVARSIZE];
  static char vbuff[CF_MAXVARSIZE];

if (reset)
   {
   if (strcmp(name,"CF_NOMATCH") == 0 || strcmp(version,"CF_NOMATCH") == 0)
      {
      return false;
      }

   if (strcmp(name,"") != 0 || strcmp(version,"") != 0)
      {
      Debug(" -? Extracted package name \"%s\"\n",name);
      Debug(" -?      with version \"%s\"\n",version);
      Debug(" -?      with architecture \"%s\"\n",arch);
      
      PrependPackageItem(list,name,version,arch,a,pp);
      }

   strcpy(name,"CF_NOMATCH");
   strcpy(version,"CF_NOMATCH");
   strcpy(arch,"default");
   }

if (FullTextMatch(a.packages.package_list_name_regex,item))
   {
   strlcpy(vbuff,ExtractFirstReference(a.packages.package_list_name_regex,item),CF_MAXVARSIZE);
   sscanf(vbuff,"%s",name); /* trim */
   }

if (FullTextMatch(a.packages.package_list_version_regex,item))
   {
   strncpy(vbuff,ExtractFirstReference(a.packages.package_list_version_regex,item),CF_MAXVARSIZE-1);
   sscanf(vbuff,"%s",version); /* trim */
   }

if (a.packages.package_list_arch_regex && FullTextMatch(a.packages.package_list_arch_regex,item))
   {
   if (a.packages.package_list_arch_regex)
      {
      strncpy(vbuff,ExtractFirstReference(a.packages.package_list_arch_regex,item),CF_MAXVARSIZE-1);
      sscanf(vbuff,"%s",arch); /* trim */
      }
   }
   
return false;
}

/*****************************************************************************/

static int PrependPatchItem(struct CfPackageItem **list,char *item,struct CfPackageItem *chklist,struct Attributes a,struct Promise *pp)

{ char name[CF_MAXVARSIZE];
  char arch[CF_MAXVARSIZE];
  char version[CF_MAXVARSIZE];
  char vbuff[CF_MAXVARSIZE];

strncpy(vbuff,ExtractFirstReference(a.packages.package_patch_name_regex,item),CF_MAXVARSIZE-1);
sscanf(vbuff,"%s",name); /* trim */
strncpy(vbuff,ExtractFirstReference(a.packages.package_patch_version_regex,item),CF_MAXVARSIZE-1);
sscanf(vbuff,"%s",version); /* trim */

if (a.packages.package_patch_arch_regex)
   {
   strncpy(vbuff,ExtractFirstReference(a.packages.package_patch_arch_regex,item),CF_MAXVARSIZE-1);
   sscanf(vbuff,"%s",arch); /* trim */
   }
else
   {
   strncpy(arch,"default",CF_MAXVARSIZE-1);
   }

if (strcmp(name,"CF_NOMATCH") == 0 || strcmp(version,"CF_NOMATCH") == 0 || strcmp(arch,"CF_NOMATCH") == 0)
   {
   return false;
   }

Debug(" ?? Patch line: \"%s\"",item);
Debug(" -?      with name \"%s\"\n",name);
Debug(" -?      with version \"%s\"\n",version);
Debug(" -?      with architecture \"%s\"\n",arch);

if (PackageInItemList(chklist,name,version,arch))
   {
   CfOut(cf_verbose,""," -> Patch for (%s,%s,%s) found, but it appears to be installed already",name,version,arch);
   return false;
   }

return PrependPackageItem(list,name,version,arch,a,pp);
}

/*****************************************************************************/

static void DeletePackageItems(struct CfPackageItem *pi)

{
if (pi)
   {
   free(pi->name);
   free(pi->version);
   free(pi->arch);
   DeletePromise(pi->pp);
   free(pi);
   }
}

/*****************************************************************************/

static int PackageMatch(char *n,char *v,char *a,struct Attributes attr,struct Promise *pp)
/*
 * Returns true if any installed packages match (n,v,a), false otherwise.
 */
{ struct CfPackageManager *mp = NULL;
  struct CfPackageItem  *pi;
  
for (mp = INSTALLED_PACKAGE_LISTS; mp != NULL; mp = mp->next)
   {
   if (strcmp(mp->manager,attr.packages.package_list_command) == 0)
      {
      break;
      }
   }

CfOut(cf_verbose,""," -> Looking for (%s,%s,%s)\n",n,v,a);

for (pi = mp->pack_list; pi != NULL; pi=pi->next)
   {
   if (ComparePackages(n,v,a,pi,attr.packages.package_select))
      {
      return true;
      }
   }

CfOut(cf_verbose,"","No installed packages matched (%s,%s,%s)\n",n,v,a);
return false;
}

/*****************************************************************************/

static int PatchMatch(char *n,char *v,char *a,struct Attributes attr,struct Promise *pp)

{ struct CfPackageManager *mp = NULL;
  struct CfPackageItem  *pi;
  
for (mp = INSTALLED_PACKAGE_LISTS; mp != NULL; mp = mp->next)
   {
   if (strcmp(mp->manager,attr.packages.package_list_command) == 0)
      {
      break;
      }
   }

CfOut(cf_verbose,""," -> Looking for (%s,%s,%s)\n",n,v,a);

for (pi = mp->patch_list; pi != NULL; pi=pi->next)
   {
   if (FullTextMatch(n,pi->name)) /* Check regexes */
      {
      return true;
      }
   else if (ComparePackages(n,v,a,pi,attr.packages.package_select))
      {
      return true;
      }
   }

CfOut(cf_verbose,""," !! Unsatisfied constraints in promise (%s,%s,%s)\n",n,v,a);
return false;
}

/*****************************************************************************/

static void SchedulePackageOp(char *name,char *version,char *arch,int installed,int matched,int no_version_specified,struct Attributes a,struct Promise *pp)

{ struct CfPackageManager *manager;
 char reference[CF_EXPANDSIZE],reference2[CF_EXPANDSIZE];
  char refAnyVer[CF_EXPANDSIZE];
  char refAnyVerEsc[CF_EXPANDSIZE];
  char largestVerAvail[CF_MAXVARSIZE];
  char largestPackAvail[CF_MAXVARSIZE];
  char instVer[CF_MAXVARSIZE];
  char instArch[CF_MAXVARSIZE];
  char idBuf[CF_MAXVARSIZE];
  char *id,*id_del;
  char *pathName = NULL;
  int package_select_in_range = false;
  enum package_actions policy;

  CfOut(cf_verbose, "", "Checking if package (%s,%s,%s) is at the desired state (installed=%d,matched=%d)",
	name, version, arch, installed, matched);
 
/* Now we need to know the name-convention expected by the package manager */

if (a.packages.package_name_convention || a.packages.package_delete_convention)
   {
   SetNewScope("cf_pack_context");
   NewScalar("cf_pack_context","name",name,cf_str);
   NewScalar("cf_pack_context","version",version,cf_str);
   NewScalar("cf_pack_context","arch",arch,cf_str);

   if(a.packages.package_delete_convention && (a.packages.package_policy == cfa_deletepack))
     {
     ExpandScalar(a.packages.package_delete_convention,reference);
     id = reference;
     }
   else if(a.packages.package_name_convention)
     {
     ExpandScalar(a.packages.package_name_convention,reference);
     id = reference;
     }
   else
     {
     id = name;
     }

   DeleteScope("cf_pack_context");
   }
else
   {
   id = name;
   }


CfOut(cf_verbose,""," -> Package promises to refer to itself as \"%s\" to the manager\n",id);

if(strchr(id, '*'))
  {
  CfOut(cf_verbose,"","!! Package name contians '*' -- perhaps a missing attribute (name/version/arch) should be specified");
  }


if (a.packages.package_select == cfa_eq || a.packages.package_select == cfa_ge ||
    a.packages.package_select == cfa_le || a.packages.package_select == cfa_cmp_none)
   {
   CfOut(cf_verbose,""," -> Package version seems to match criteria");
   package_select_in_range = true;
   }


 policy = a.packages.package_policy;

 if(policy == cfa_addupdate)
   {
   if(!installed)
     {
     policy = cfa_addpack;
     }
   else
     {
     policy = cfa_update;
     }
   }

switch(policy)
   {
   case cfa_addpack:

       if (installed == 0)
          {
          if((a.packages.package_file_repositories != NULL) &&
             (a.packages.package_select == cfa_gt || a.packages.package_select == cfa_ge))
             {
             
             SetNewScope("cf_pack_context_anyver");
             NewScalar("cf_pack_context_anyver","name",name,cf_str);
             NewScalar("cf_pack_context_anyver","version","(.*)",cf_str);
             NewScalar("cf_pack_context_anyver","arch",arch,cf_str);
             ExpandScalar(a.packages.package_name_convention,refAnyVer);
             DeleteScope("cf_pack_context_anyver");
             
             
             EscapeSpecialChars(refAnyVer, refAnyVerEsc, sizeof(refAnyVerEsc), "(.*)");
             
             if(FindLargestVersionAvail(largestPackAvail, largestVerAvail, refAnyVerEsc, version, a.packages.package_select, a.packages.package_file_repositories))
                {
                CfOut(cf_verbose, "", "Using latest version in file repositories; \"%s\"", largestPackAvail);
                id = largestPackAvail;
                }
             else
                {
                CfOut(cf_verbose, "", "No package in file repositories satisfy version constraint");
                break;
                }
             }          
          
          CfOut(cf_verbose,""," -> Schedule package for addition\n");
          manager = NewPackageManager(&PACKAGE_SCHEDULE,a.packages.package_add_command,cfa_addpack,a.packages.package_changes);
          PrependPackageItem(&(manager->pack_list),id,"any","any",a,pp);
          }
       else
          {
          cfPS(cf_verbose,CF_NOP,"",pp,a," -> Package \"%s\" already installed, so we never add it again\n",pp->promiser);
          }
       break;
       
   case cfa_deletepack:

       if (matched && package_select_in_range || installed && no_version_specified)
          {
          CfOut(cf_verbose,""," -> Schedule package for deletion\n");

	  // expand local repository in the name convetion, if present
	  if(a.packages.package_file_repositories)
	    {
	    // remove any "$(repo)" from the name convention string

	    if (strncmp(id, "$(firstrepo)", 12) == 0)
               {
               snprintf(idBuf, sizeof(idBuf), "%s", id + 12);
               
               // and add the correct repo
               pathName = PrefixLocalRepository(a.packages.package_file_repositories, idBuf);
               
               if(pathName)
                  {
		  snprintf(id, CF_MAXVARSIZE, "%s", pathName);
		  CfOut(cf_verbose, "", "Expanded the package repository to %s", id);
                  }
               else
                  {
		  CfOut(cf_error, "", "!! Package \"%s\" can't be found in any of the listed repositories", idBuf);
                  }
               }
	    }
          
          manager = NewPackageManager(&PACKAGE_SCHEDULE,a.packages.package_delete_command,cfa_deletepack,a.packages.package_changes);
          PrependPackageItem(&(manager->pack_list),id,"any","any",a,pp);
          }
       else
          {
          cfPS(cf_verbose,CF_NOP,"",pp,a," -> Package deletion is as promised -- no match\n");
          }
       
       break;
       
   case cfa_reinstall:

       if (a.packages.package_delete_command == NULL)
          {
          cfPS(cf_verbose,CF_FAIL,"",pp,a,"Package delete command undefined");
          return;
          }

       if (a.packages.package_add_command == NULL)
          {
          cfPS(cf_verbose,CF_FAIL,"",pp,a,"Package add command undefined");
          return;
          }

       if (!no_version_specified)
          {
          CfOut(cf_verbose,""," -> Schedule package for reinstallation\n");
          if (matched && package_select_in_range || installed && no_version_specified)
             {
             manager = NewPackageManager(&PACKAGE_SCHEDULE,a.packages.package_delete_command,cfa_deletepack,a.packages.package_changes);
             PrependPackageItem(&(manager->pack_list),id,"any","any",a,pp);
             }
          manager = NewPackageManager(&PACKAGE_SCHEDULE,a.packages.package_add_command,cfa_addpack,a.packages.package_changes);
          PrependPackageItem(&(manager->pack_list),id,"any","any",a,pp);
          }
       else
          {
          cfPS(cf_error,CF_FAIL,"",pp,a,"!! Package reinstallation cannot be promised -- insufficient version info or no match\n");
          }
       
       break;
       
   case cfa_update:

       *instVer = '\0';
       *instArch = '\0';
       
       if ((a.packages.package_file_repositories != NULL) &&
	  (a.packages.package_select == cfa_gt || a.packages.package_select == cfa_ge))
          {          
          SetNewScope("cf_pack_context_anyver");
          NewScalar("cf_pack_context_anyver","name",name,cf_str);
          NewScalar("cf_pack_context_anyver","version","(.*)",cf_str);
          NewScalar("cf_pack_context_anyver","arch",arch,cf_str);
          ExpandScalar(a.packages.package_name_convention,refAnyVer);
          DeleteScope("cf_pack_context_anyver");
                    
          EscapeSpecialChars(refAnyVer, refAnyVerEsc, sizeof(refAnyVerEsc), "(.*)");
	  
          if (FindLargestVersionAvail(largestPackAvail, largestVerAvail, refAnyVerEsc, version, a.packages.package_select, a.packages.package_file_repositories))
	     {
             CfOut(cf_verbose, "", "Using latest version in file repositories; \"%s\"", largestPackAvail);
             id = largestPackAvail;
	     }
          else
	     {
             CfOut(cf_verbose, "", "No package in file repositories satisfy version constraint");
             break;
	     }
          }
       else
          {
          snprintf(largestVerAvail, sizeof(largestVerAvail), "%s", version);  // user-supplied version
          }
       
       
       if (installed)
          {
          CfOut(cf_verbose, "", "Checking if latest available version is newer than installed...");
	  
          if (IsNewerThanInstalled(name, largestVerAvail, arch, instVer, instArch, a))
             {
             CfOut(cf_verbose, "", "Installed package (%s,%s,%s) is older than latest available (%s,%s,%s) - updating",
                   name, instVer, instArch, name, largestVerAvail, arch);
             }
          else
             {
             CfOut(cf_verbose, "", "Installed package is up to date, not updating");
             break;
             }
          }
       
       
       if ((matched && package_select_in_range && !no_version_specified) || installed)
          {
          if (a.packages.package_update_command == NULL)
             {
             CfOut(cf_verbose,""," !! Package update command undefined - failing over to delete then add");

	     // we need to have the version of installed package
	     if (a.packages.package_delete_convention)
                {
                if (*instVer == '\0')
		   {
                   instVer[0] = '*';
                   instVer[1] = '\0';
		   }
                
                if (*instArch == '\0')
		   {
                   instArch[0] = '*';
                   instArch[1] = '\0';
		   }
                
                SetNewScope("cf_pack_context");
                NewScalar("cf_pack_context","name",name,cf_str);
                NewScalar("cf_pack_context","version",instVer,cf_str);
                NewScalar("cf_pack_context","arch",instArch,cf_str);
                ExpandScalar(a.packages.package_delete_convention,reference2);
                id_del = reference2;
                DeleteScope("cf_pack_context");
                }
	     else
                {
                id_del = id; // defaults to the package_name_convention
                }
             
             
	     CfOut(cf_verbose, "", "Scheduling package with id \"%s\" for deletion", id_del);
             
             manager = NewPackageManager(&PACKAGE_SCHEDULE,a.packages.package_delete_command,cfa_deletepack,a.packages.package_changes);
             PrependPackageItem(&(manager->pack_list),id_del,"any","any",a,pp);

             manager = NewPackageManager(&PACKAGE_SCHEDULE,a.packages.package_add_command,cfa_addpack,a.packages.package_changes);
             PrependPackageItem(&(manager->pack_list),id,"any","any",a,pp);
             }
          else
             {
             CfOut(cf_verbose,""," -> Schedule package for update\n");
             manager = NewPackageManager(&PACKAGE_SCHEDULE,a.packages.package_update_command,cfa_update,a.packages.package_changes);
             PrependPackageItem(&(manager->pack_list),id,"any","any",a,pp);
             }
          }
       else
          {
          cfPS(cf_error,CF_FAIL,"",pp,a,"!! Package \"%s\" cannot be updated -- no match or not installed",pp->promiser);
          }
       break;
       
   case cfa_patch:

       if (matched && !installed)
          {
          CfOut(cf_verbose,""," -> Schedule package for patching\n");
          manager = NewPackageManager(&PACKAGE_SCHEDULE,a.packages.package_patch_command,cfa_patch,a.packages.package_changes);
          PrependPackageItem(&(manager->patch_list),id,"any","any",a,pp);
          }
       else
          {
          cfPS(cf_verbose,CF_NOP,"",pp,a," -> Package patch state of \"%s\" is as promised -- already installed\n",pp->promiser);
          }
       break;

   case cfa_verifypack:
       
       if (matched && package_select_in_range || installed && no_version_specified)
          {
          CfOut(cf_verbose,""," -> Schedule package for verification\n");
          manager = NewPackageManager(&PACKAGE_SCHEDULE,a.packages.package_verify_command,cfa_verifypack,a.packages.package_changes);
          PrependPackageItem(&(manager->pack_list),id,"any","any",a,pp);
          }
       else
          {
          cfPS(cf_inform,CF_FAIL,"",pp,a,"!! Package \"%s\" cannot be verified -- no match\n",pp->promiser);
          }
       
       break;
       
   default:
       break;
   }
}

/*****************************************************************************/

char *PrefixLocalRepository(struct Rlist *repositories,char *package)

{ 
  static char quotedPath[CF_MAXVARSIZE];
  struct Rlist *rp;
  struct stat sb;
  char path[CF_BUFSIZE];

  
for (rp = repositories; rp != NULL; rp=rp->next)
   {
   strncpy(path,rp->item,CF_MAXVARSIZE);

   AddSlash(path);

   strcat(path,package);

   if (cfstat(path,&sb) != -1)
      {
      snprintf(quotedPath, sizeof(quotedPath), "\"%s\"", path);
      return quotedPath;
      }
   }

return NULL;
}

/*****************************************************************************/

int FindLargestVersionAvail(char *matchName, char *matchVers, char *refAnyVer, char *ver, enum version_cmp package_select, struct Rlist *repositories)
/* Returns true if a version gt/ge ver is found in local repos, false otherwise */
{
  struct Rlist *rp;
  const struct dirent *dirp;
  char largestVer[CF_MAXVARSIZE];
  char largestVerName[CF_MAXVARSIZE];
  char *matchVer;
  int match;
  CFDIR *dirh;

  Debug("FindLargestVersionAvail()\n");

  match = false;

  // match any version
  if((strlen(ver) == 0) || (strcmp(ver, "*") == 0))
    {
      memset(largestVer, 0, sizeof(largestVer));
    }
  else
    {
      snprintf(largestVer, sizeof(largestVer), "%s", ver);

      if(package_select == cfa_gt)  // either gt or ge
	{
	  largestVer[strlen(largestVer) - 1]++;
	}
    }

  for (rp = repositories; rp != NULL; rp=rp->next)
    {

      if ((dirh = OpenDirLocal(rp->item)) == NULL)
	{
	  CfOut(cf_error,"opendir","!! Can't open local directory \"%s\"\n", rp->item);
	  continue;
	}
      
      for (dirp = ReadDir(dirh); dirp != NULL; dirp = ReadDir(dirh))
	{
#ifdef LINUX 
	  if(dirp->d_type != DT_REG && dirp->d_type != DT_LNK)
	    {
	    CfOut(cf_verbose, "", "Skipping \"%s\" (not a file)", dirp->d_name);
	    continue;
	    }
#endif  /* LINUX */
          
	  if (FullTextMatch(refAnyVer, dirp->d_name))
             {
             matchVer = ExtractFirstReference(refAnyVer, dirp->d_name);
             
             // check if match is largest so far
             if(VersionCmp(largestVer, matchVer))
		{
                snprintf(largestVer, sizeof(largestVer), "%s", matchVer);
                snprintf(largestVerName, sizeof(largestVerName), "%s", dirp->d_name);
                match = true;
		}
             }
	  
	}
      
    CloseDir(dirh);
    }
  
  Debug("largest ver is \"%s\", name is \"%s\"\n", largestVer, largestVerName);
  Debug("match=%d\n", match);
  
  if(match)
    {
    snprintf(matchName, CF_MAXVARSIZE, "%s", largestVerName);
    snprintf(matchVers, CF_MAXVARSIZE, "%s", largestVer);
    }
  
return match;
}

/*****************************************************************************/

int VersionCmp(char *vs1,char *vs2)

/* Returns true if vs2 is a larger or equal version than vs1, false otherwise */

{ int i;
  char ch1,ch2;
  
if (strlen(vs1) > strlen(vs2))
   {
   return false;
   }

if (strlen(vs1) < strlen(vs2))
   {
   return true;
   }

for (i = 0; i < strlen(vs1); i++)
   {
   ch1 = (vs1[i] == ',') ? '_' : vs1[i]; // CSV protection. Names will be canonified of commas
   ch2 = (vs2[i] == ',') ? '_' : vs2[i]; // CSV protection. Names will be canonified of commas
   
   if (ch1 < ch2)
      {
      return true;
      }
   else if (ch1 > ch2)
      {
      return false;
      }
   }

return true;
}

/*****************************************************************************/

static int IsNewerThanInstalled(char *n,char *v,char *a, char *instV, char *instA, struct Attributes attr)

/* Returns true if a package (n, a) is installed and v is larger than
 * the installed version. instV and instA are the version and arch installed. */

{ struct CfPackageManager *mp = NULL;
  struct CfPackageItem  *pi;
  
for (mp = INSTALLED_PACKAGE_LISTS; mp != NULL; mp = mp->next)
   {
   if (strcmp(mp->manager,attr.packages.package_list_command) == 0)
      {
      break;
      }
   }

CfOut(cf_verbose,"","Looking for an installed package older than (%s,%s,%s)",n,v,a);

for (pi = mp->pack_list; pi != NULL; pi=pi->next)
   {
   if ((strcmp(n, pi->name) == 0) && ((strcmp(a, pi->arch) == 0) || (strcmp("default", pi->arch) == 0)))
      {
      CfOut(cf_verbose, "", "Found installed package (%s,%s,%s)", pi->name, pi->version, pi->arch);
      
      snprintf(instV, CF_MAXVARSIZE, "%s", pi->version);
      snprintf(instA, CF_MAXVARSIZE, "%s", pi->arch);
      
      if (!VersionCmp(v,pi->version))
         {
         return true;
         }
      else
         {
         return false;
         }
      }
   }

CfOut(cf_verbose,""," !! Package (%s,%s) is not installed\n",n,a);
return false;
}

/*****************************************************************************/

static int ExecPackageCommand(char *command,int verify,int setCmdClasses,struct Attributes a,struct Promise *pp)

{
if(strncmp(command,"/cf_internal_rpath",sizeof("/cf_internal_rpath") - 1) == 0)
   {
   return ExecPackageCommandRpath(command,verify,setCmdClasses,a,pp);
   }
else
   {
   return ExecPackageCommandGeneric(command,verify,setCmdClasses,a,pp);
   }
}

/*****************************************************************************/

int ExecPackageCommandGeneric(char *command,int verify,int setCmdClasses,struct Attributes a,struct Promise *pp)

{ int retval = true;
 char line[CF_BUFSIZE], lineSafe[CF_BUFSIZE], *cmd; 
 FILE *pfp;
 int packmanRetval = 0;

 if (!IsExecutable(GetArg0(command)))
    {
    cfPS(cf_error,CF_FAIL,"",pp,a,"The proposed package schedule command \"%s\" was not executable",command);
    return false;
    }

 if (DONTDO)
    {
    CfOut(cf_error,""," -> Need to execute %-.39s...\n",command);
    return true;
    }

/* Use this form to avoid limited, intermediate argument processing - long lines */

 if ((pfp = cf_popen_sh(command,"r")) == NULL)
    {
    cfPS(cf_error,CF_FAIL,"cf_popen",pp,a,"Couldn't start command %20s...\n",command);
    return false;
    }

 CfOut(cf_verbose,"","Executing %-.60s...\n",command);

/* Look for short command summary */
 for (cmd = command; *cmd != '\0' && *cmd != ' '; cmd++)
    {
    }

 while (*(cmd-1) != FILE_SEPARATOR && cmd >= command)
    {
    cmd--;
    }

 while (!feof(pfp))
    {
    if (ferror(pfp))  /* abortable */
       {
       fflush(pfp);
       cfPS(cf_error,CF_INTERPT,"read",pp,a,"Couldn't start command %20s...\n",command);
       break;
       }

    line[0] = '\0';
    CfReadLine(line,CF_BUFSIZE-1,pfp);
   
    ReplaceStr(line,lineSafe,sizeof(lineSafe),"%","%%");
    CfOut(cf_inform,"","Q:%20.20s ...:%s",cmd,lineSafe);

    if (verify && line[0] != '\0')
       {
       if (a.packages.package_noverify_regex)
          {
          if(FullTextMatch(a.packages.package_noverify_regex,line))
             {
             cfPS(cf_inform,CF_FAIL,"",pp,a,"Package verification error in %-.40s ... :%s",cmd,lineSafe);
             retval = false;
             }
          }
       }
   
    }

 packmanRetval = cf_pclose(pfp);

 if(verify && a.packages.package_noverify_returncode != CF_NOINT)
    {
    if(a.packages.package_noverify_returncode == packmanRetval)
       {
       cfPS(cf_inform,CF_FAIL,"",pp,a,"!! Package verification error (returned %d)",packmanRetval);
       retval = false;
       }
    else
       {
       cfPS(cf_inform,CF_NOP,"",pp,a,"-> Package verification succeeded (returned %d)",packmanRetval);
       }
    }
 else if(verify && a.packages.package_noverify_regex)
    {
    if(retval)  // set status if we succeeded above
       {
       cfPS(cf_inform,CF_NOP,"",pp,a,"-> Package verification succeeded (no match with package_noverify_regex)");
       }
    }
 else if(setCmdClasses)  // generic return code check
    {
    retval = VerifyCommandRetcode(packmanRetval,true,a,pp);
    }

 return retval; 
}

/*****************************************************************************/
/* Level                                                                     */
/*****************************************************************************/

static int ComparePackages(char *n,char *v,char *a,struct CfPackageItem *pi,enum version_cmp cmp)

{ struct Rlist *numbers_pr = NULL,*separators_pr = NULL;
  struct Rlist *numbers_in = NULL,*separators_in = NULL;
  struct Rlist *rp_pr,*rp_in;
  int result = true;
  int version_matched = false;
  int version_equal = false;
  int cmp_result;
  int break_loop = false;

Debug("Compare (%s,%s,%s) and (%s,%s,%s)\n",n,v,a,pi->name,pi->version,pi->arch);
  
if (CompareCSVName(n,pi->name) != 0)
   {
   return false;
   }

CfOut(cf_verbose,""," -> Matched name %s\n",n);

if (strcmp(a,"*") != 0)
   {
   if (strcmp(a,pi->arch) != 0)
      {
      return false;
      }
   
   CfOut(cf_verbose,""," -> Matched arch %s\n",a);
   }

if (strcmp(v,"*") == 0)
   {   
   CfOut(cf_verbose,""," -> Matched version *\n");
   return true;
   }

ParsePackageVersion(CanonifyChar(v,','),&numbers_pr,&separators_pr);
ParsePackageVersion(CanonifyChar(pi->version,','),&numbers_in,&separators_in);

/* If the format of the version string doesn't match, we're already doomed */

CfOut(cf_verbose,""," -> Check for compatible versioning model in (%s,%s)\n",v,pi->version);

for (rp_pr = separators_pr,rp_in = separators_in; rp_pr != NULL && rp_in != NULL; rp_pr= rp_pr->next,rp_in=rp_in->next)
   {
   if (strcmp(rp_pr->item,rp_in->item) != 0)
      {
      result = false;
      break;
      }
   
   if (rp_pr->next == NULL && rp_in->next == NULL)
      {
      result = true;
      break;
      }

   if ((rp_pr->next != NULL && rp_in->next == NULL) || (rp_pr->next == NULL && rp_in->next != NULL))
      {
      result = false;
      break;
      }
   }

if (result)
   {
   CfOut(cf_verbose,""," -> Verified that versioning models are compatible\n");
   }
else
   {
   CfOut(cf_verbose,""," !! Versioning models for (%s,%s) were incompatible\n",v,pi->version);
   }

version_equal = (strcmp(pi->version,v) == 0);

if (result)
   {
   for (rp_pr = numbers_pr,rp_in = numbers_in; rp_pr != NULL && rp_in != NULL; rp_pr= rp_pr->next,rp_in=rp_in->next)
      {
      cmp_result = strcmp(rp_pr->item,rp_in->item);

      switch (cmp)
         {
         case cfa_eq:
         case cfa_cmp_none:
             if (version_equal)
                {
                version_matched = true;
                }             
             break;
         case cfa_neq:
             if (!version_equal)
                {
                version_matched = true;
                }             
             break;
         case cfa_gt:
             if (cmp_result < 0)
                {
                version_matched = true;
                }             
             else if (cmp_result > 0)
                {
                break_loop = true;
                }
             break;
         case cfa_lt:
             if (cmp_result > 0)
                {
                version_matched = true;
                }             
             else if (cmp_result < 0)
                {
                break_loop = true;
                }
             break;
         case cfa_ge:
             if ((cmp_result < 0) || version_equal)
                {
                version_matched = true;
                }             
             else if (cmp_result > 0)
                {
                break_loop = true;
                }
             break;
         case cfa_le:
             if ((cmp_result > 0) || version_equal)
                {
                version_matched = true;
                }             
             else if (cmp_result < 0)
                {
                break_loop = true;
                }
             break;
         default:
             break;
         }

      if ((version_matched == true) || break_loop)
         {
         break;
         }
      }
   }

result = version_matched;

if (result)
   {
   CfOut(cf_verbose,""," -> Verified version constraint promise kept\n");
   }
else
   {
   CfOut(cf_verbose,""," -> Versions did not match\n");
   }

DeleteRlist(numbers_pr);
DeleteRlist(numbers_in);
DeleteRlist(separators_pr);
DeleteRlist(separators_in);
return result;
}

/*****************************************************************************/

static int PackageInItemList(struct CfPackageItem *list,char *name,char *version,char *arch)

{ struct CfPackageItem *pi;
 
if (strlen(name) == 0 || strlen(version) == 0 || strlen(arch) == 0)
   {
   return false;
   }

for (pi = list; pi != NULL; pi = pi->next)
   {
   if (strcmp(pi->name,name) == 0 && strcmp(pi->version,version) == 0 && strcmp(pi->arch,arch) == 0)
      {
      return true;
      }
   }

return false;
}


/*****************************************************************************/
/* Level                                                                     */
/*****************************************************************************/

static void ParsePackageVersion(char *version,struct Rlist **num,struct Rlist **sep)

{ char *sp,numeral[30],separator[2];

if (version == NULL)
   {
   return;
   }
 
for (sp = version; *sp != '\0'; sp++)
   {
   memset(numeral,0,30);
   memset(separator,0,2);

   /* Split in 2's complement */
   
   sscanf(sp,"%29[0-9a-zA-Z]",numeral);
   sp += strlen(numeral);

   /* Append to end up with left->right (major->minor) comparison */

   AppendRScalar(num,numeral,CF_SCALAR);

   if (*sp == '\0')
      {
      return;
      }
   
   sscanf(sp,"%1[^0-9a-zA-Z]",separator);
   PrependRScalar(sep,separator,CF_SCALAR);
   }
}

/*****************************************************************************/

static void InvalidateSoftwareCache(void)
{
char name[CF_BUFSIZE];
GetSoftwareCacheFilename(name);

struct utimbuf epoch = { 0, 0 };

if (utime(name, &epoch) != 0)
   {
   if (errno != ENOENT)
      {
      CfOut(cf_error, "utimes", "Cannot mark software cache as invalid");
      }
   }
}

/*****************************************************************************/

static void ReportSoftware(struct CfPackageManager *list)

{ FILE *fout;
  struct CfPackageManager *mp = NULL;
  struct CfPackageItem *pi;
  char name[CF_BUFSIZE];

GetSoftwareCacheFilename(name);

if ((fout = fopen(name,"w")) == NULL)
   {
   CfOut(cf_error,"fopen","Cannot open the destination file %s",name);
   return;
   }

for (mp = list; mp != NULL; mp = mp->next)
   {
   for (pi = mp->pack_list; pi != NULL; pi=pi->next)
      {
      fprintf(fout,"%s,",CanonifyChar(pi->name,','));
      fprintf(fout,"%s,",CanonifyChar(pi->version,','));
      fprintf(fout,"%s,%s\n",pi->arch,ReadLastNode(GetArg0(mp->manager)));
      }
   }

fclose(fout);
}

/*****************************************************************************/

/* Buffer should be at least CF_BUFSIZE large */
static const char *GetSoftwareCacheFilename(char *buffer)
{
snprintf(buffer,CF_BUFSIZE,"%s/state/%s",CFWORKDIR,SOFTWARE_PACKAGES_CACHE);
MapName(buffer);
return buffer;
}