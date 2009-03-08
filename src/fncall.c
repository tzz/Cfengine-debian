/* 
   Copyright (C) 2008 - Cfengine AS

   This file is part of Cfengine 3 - written and maintained by Cfengine AS.
 
   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; either version 3, or (at your option) any
   later version. 
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

*/

/*****************************************************************************/
/*                                                                           */
/* File: fncall.c                                                            */
/*                                                                           */
/* Created: Wed Aug  8 14:45:53 2007                                         */
/*                                                                           */
/*****************************************************************************/

#include "cf3.defs.h"
#include "cf3.extern.h"

extern struct FnCallType CF_FNCALL_TYPES[];

/*******************************************************************/

int IsBuiltinFnCall(void *rval,char rtype)

{ int i;
  struct FnCall *fp;

if (rtype != CF_FNCALL)
   {
   return false;
   }

fp = (struct FnCall *)rval;

for (i = 0; CF_FNCALL_TYPES[i].name != NULL; i++)
   {
   if (strcmp(CF_FNCALL_TYPES[i].name,fp->name) == 0)
      {
      Debug("%s is a builtin function\n",fp->name);
      return true;
      }
   }

return false;
}

/*******************************************************************/

struct FnCall *NewFnCall(char *name, struct Rlist *args)

{ struct FnCall *fp;
  char *sp = NULL;
  struct Rlist *rp;

Debug("Installing Function Call %s\n",name);

if ((fp = (struct FnCall *)malloc(sizeof(struct FnCall))) == NULL)
   {
   CfOut(cf_error,"malloc","Unable to allocate FnCall");
   FatalError("");
   }

if ((sp = strdup(name)) == NULL)
   {
   CfOut(cf_error,"malloc","Unable to allocate Promise");
   FatalError("");
   }

fp->name = sp;
fp->args = args;
fp->argc = RlistLen(args);

Debug("Installed ");
if (DEBUG)
   {
   ShowFnCall(stdout,fp);
   }
Debug("\n\n");
return fp;
}

/*******************************************************************/

struct FnCall *CopyFnCall(struct FnCall *f)

{
return NewFnCall(f->name,CopyRlist(f->args));
}

/*******************************************************************/

void DeleteFnCall(struct FnCall *fp)

{
Debug("DeleteFnCall %s\n",fp->name);

if (fp->name)
   {
   free(fp->name);
   }

if (fp->args)
   {
   DeleteRvalItem(fp->args,CF_LIST);
   }

free(fp);
}

/*********************************************************************/

struct FnCall *ExpandFnCall(char *contextid,struct FnCall *f,int expandnaked)

{
//return NewFnCall(f->name,ExpandList(contextid,f->args,expandnaked));
 return NewFnCall(f->name,ExpandList(contextid,f->args,false));
}

/*******************************************************************/

void PrintFunctions()

{ struct FnCall *fp;
 int i;

 for (i = 0; i < 3; i++)
    {
    if (P.currentfncall[i] != NULL)
       {
       printf("(%d) =========================\n|",i);
       ShowFnCall(stdout,P.currentfncall[i]);
       printf("|\n==============================\n");
       }
    } 
}


/*******************************************************************/

void ShowFnCall(FILE *fout,struct FnCall *fp)

{ struct Rlist *rp;

if (XML)
   {
   fprintf(fout,"%s(",fp->name);
   }
else
   {
   fprintf(fout,"%s(",fp->name);
   }

for (rp = fp->args; rp != NULL; rp=rp->next)
   {
   switch (rp->type)
      {
      case CF_SCALAR:
          fprintf(fout,"%s,",(char *)rp->item);
          break;

      case CF_FNCALL:
          ShowFnCall(fout,(struct FnCall *)rp->item);
          break;

      default:
          fprintf(fout,"(** Unknown argument **)\n");
          break;
      }
   }

if (XML)
   {
   fprintf(fout,")");
   }
else
   {
   fprintf(fout,")");
   }
}

/*******************************************************************/

enum cfdatatype FunctionReturnType(char *name)

{ struct FnCallType fncall;
  int i;

for (i = 0; CF_FNCALL_TYPES[i].name != NULL; i++)
   {
   if (strcmp(name,CF_FNCALL_TYPES[i].name) == 0)
      {
      return CF_FNCALL_TYPES[i].dtype;
      }
   }

return cf_notype;
}

/*******************************************************************/

struct Rval EvaluateFunctionCall(struct FnCall *fp,struct Promise *pp)

{ struct Rlist *expargs;
  struct Rval rval;
  enum fncalltype this = FnCallName(fp->name);

rval.item = NULL;
rval.rtype = CF_NOPROMISEE;

if (this != cfn_unknown)
   {
   if (DEBUG)
      {
      printf("EVALUATE FN CALL %s\n",fp->name);
      ShowFnCall(stdout,fp);
      printf("\n");
      }
   }
else
   {
   if (pp)
      {
      CfOut(cf_error,"","No such FnCall \"%s()\" in promise @ %s near line %d\n",fp->name,pp->audit->filename,pp->lineno);
      }
   else
      {
      CfOut(cf_error,"","No such FnCall \"%s()\" - context info unavailable\n",fp->name);
      }
   
   return rval;
   }

ClearFnCallStatus();

expargs = NewExpArgs(fp,pp);

switch (this)
   {
   case cfn_canonify:
       rval = FnCallCanonify(fp,expargs);
       break;
   case cfn_randomint:
       rval = FnCallRandomInt(fp,expargs);
       break;
   case cfn_getuid:
       rval = FnCallGetUid(fp,expargs);
       break;
   case cfn_getgid:
       rval = FnCallGetGid(fp,expargs);
       break;
   case cfn_execresult:
       rval = FnCallExecResult(fp,expargs);
       break;
   case cfn_readtcp:
       rval = FnCallReadTcp(fp,expargs);
       break;
   case cfn_returnszero:
       rval = FnCallReturnsZero(fp,expargs);
       break;
   case cfn_isnewerthan:
       rval = FnCallIsNewerThan(fp,expargs);
       break;
   case cfn_accessedbefore:
       rval = FnCallIsAccessedBefore(fp,expargs);
       break;
   case cfn_changedbefore:
       rval = FnCallIsChangedBefore(fp,expargs);
       break;
   case cfn_fileexists:
       rval = FnCallStatInfo(fp,expargs,this);
       break;
   case cfn_isdir:
       rval = FnCallStatInfo(fp,expargs,this);
       break;
   case cfn_islink:
       rval = FnCallStatInfo(fp,expargs,this);
       break;
   case cfn_isplain:
       rval = FnCallStatInfo(fp,expargs,this);
       break;
   case cfn_iprange:
       rval = FnCallIPRange(fp,expargs);
       break;
   case cfn_hostrange:
       rval = FnCallHostRange(fp,expargs);
       break;
   case cfn_hostinnetgroup:
       rval = FnCallHostInNetgroup(fp,expargs);
       break;
   case cfn_isvariable:
       rval = FnCallIsVariable(fp,expargs);
       break;
   case cfn_strcmp:
       rval = FnCallStrCmp(fp,expargs);
       break;
   case cfn_splitstring:
       rval = FnCallSplitString(fp,expargs);
       break;
   case cfn_regcmp:
       rval = FnCallRegCmp(fp,expargs);
       break;
   case cfn_regline:
       rval = FnCallRegLine(fp,expargs);
       break;
   case cfn_reglist:
       rval = FnCallRegList(fp,expargs);
       break;
   case cfn_regarray:
       rval = FnCallRegArray(fp,expargs);
       break;
   case cfn_getindices:
       rval = FnCallGetIndices(fp,expargs);
       break;
   case cfn_isgreaterthan:
       rval = FnCallGreaterThan(fp,expargs,'+');
       break;
   case cfn_islessthan:
       rval = FnCallGreaterThan(fp,expargs,'-');
       break;
   case cfn_userexists:
       rval = FnCallUserExists(fp,expargs);
       break;
   case cfn_groupexists:
       rval = FnCallGroupExists(fp,expargs);
       break;
   case cfn_readfile:
       rval = FnCallReadFile(fp,expargs);
       break;
   case cfn_readstringlist:
       rval = FnCallReadStringList(fp,expargs,cf_str);
       break;
   case cfn_readintlist:
       rval = FnCallReadStringList(fp,expargs,cf_int);
       break;
   case cfn_readreallist:
       rval = FnCallReadStringList(fp,expargs,cf_real);
       break;
   case cfn_readstringarray:
       rval = FnCallReadStringArray(fp,expargs,cf_str);
       break;
   case cfn_readintarray:
       rval = FnCallReadStringArray(fp,expargs,cf_int);
       break;
   case cfn_readrealarray:
       rval = FnCallReadStringArray(fp,expargs,cf_real);
       break;
   case cfn_irange:
       rval = FnCallIRange(fp,expargs);
       break;
   case cfn_rrange:
       rval = FnCallRRange(fp,expargs);
       break;
   case cfn_date:
       rval = FnCallOnDate(fp,expargs);
       break;
   case cfn_ago:
       rval = FnCallAgoDate(fp,expargs);
       break;
   case cfn_accum:
       rval = FnCallAccumulatedDate(fp,expargs);
       break;
   case cfn_now:
       rval = FnCallNow(fp,expargs);
       break;
   case cfn_classmatch:
       rval = FnCallClassMatch(fp,expargs);
       break;
   case cfn_hash:
       rval = FnCallHash(fp,expargs);
       break;
   case cfn_usemodule:
       rval = FnCallUseModule(fp,expargs);
       break;

   case cfn_selectservers:
       rval = FnCallSelectServers(fp,expargs);
       break;
       
   case cfn_unknown:
       CfOut(cf_error,"","Un-registered function call");
       PromiseRef(cf_error,pp);
       break;
   }


if (FNCALL_STATUS.status == FNCALL_FAILURE)
   {
   /* We do not assign variables to failed function calls */
   rval.item = CopyFnCall(fp);
   rval.rtype = CF_FNCALL;
   }

DeleteExpArgs(expargs);
return rval;
}
                
/*******************************************************************/

enum fncalltype FnCallName(char *name)

{ int i;
 
for (i = 0; CF_FNCALL_TYPES[i].name != NULL; i++)
   {
   if (strcmp(CF_FNCALL_TYPES[i].name,name) == 0)
      {
      return (enum fncalltype)i;
      }
   }

return cfn_unknown;
}

/*****************************************************************************/

void ClearFnCallStatus()

{
FNCALL_STATUS.status = CF_NOP;
FNCALL_STATUS.message[0] = '\0';
FNCALL_STATUS.fncall_classes[0] = '\0';
}

/*****************************************************************************/

void SetFnCallReturnStatus(char *name,int status,char *message,char *fncall_classes)

{
FNCALL_STATUS.status = status;

if (message && strlen(message) > 0)
   {
   strncpy(FNCALL_STATUS.message,message,CF_BUFSIZE-1);
   }

if (fncall_classes && strlen(fncall_classes))
   {
   strncpy(FNCALL_STATUS.fncall_classes,fncall_classes,CF_BUFSIZE-1);
   AddPrefixedClasses(name,fncall_classes);
   }
}
