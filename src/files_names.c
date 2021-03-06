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
/* File: files_names.c                                                       */
/*                                                                           */
/*****************************************************************************/

#include "cf3.defs.h"
#include "cf3.extern.h"

static void DeEscapeFilename(char *in,char *out);
static int JoinFast(char *path,char *leaf,char **nextFree,int bufsize);
static int StartJoinFast(char *path,char *leaf,char **nextFree,int bufsize);

/*****************************************************************************/

void LocateFilePromiserGroup(char *wildpath,struct Promise *pp,void (*fnptr)(char *path, struct Promise *ptr))

{ struct Item *path,*ip,*remainder = NULL;
  char pbuffer[CF_BUFSIZE];
  struct stat statbuf;
  int count = 0,lastnode = false, expandregex = false;
  uid_t agentuid = getuid();
  int create = GetBooleanConstraint("create",pp);
  char *pathtype = GetConstraint("pathtype",pp,CF_SCALAR);

Debug("LocateFilePromiserGroup(%s)\n",wildpath);

/* Do a search for promiser objects matching wildpath */

if (!IsPathRegex(wildpath) || (pathtype && (strcmp(pathtype,"literal") == 0)))
   {
   CfOut(cf_verbose,""," -> Using literal pathtype for %s\n",wildpath);
   (*fnptr)(wildpath,pp);
   return;
   }
else
   {
   CfOut(cf_verbose,""," -> Using regex pathtype for %s (see pathtype)\n",wildpath);
   }

pbuffer[0] = '\0';
path = SplitString(wildpath,'/');  // require forward slash in regex on all platforms

for (ip = path; ip != NULL; ip=ip->next)
   {
   if (ip->name == NULL || strlen(ip->name) == 0)
      {
      continue;
      }

   if (ip->next == NULL)
      {
      lastnode = true;
      }

   /* No need to chdir as in recursive descent, since we know about the path here */
   
   if (IsRegex(ip->name))
      {
      remainder = ip->next;
      expandregex = true;
      break;
      }
   else
      {
      expandregex = false;
      }

   if (!JoinPath(pbuffer,ip->name))
      {
      CfOut(cf_error,"","Buffer has limited size in LocateFilePromiserGroup\n");
      return;
      }

   if (cfstat(pbuffer,&statbuf) != -1)
      {
      if (S_ISDIR(statbuf.st_mode) && statbuf.st_uid != agentuid && statbuf.st_uid != 0)
         {
         CfOut(cf_inform,"","Directory %s in search path %s is controlled by another user (uid %d) - trusting its content is potentially risky (possible race)\n",pbuffer,wildpath,statbuf.st_uid);
         PromiseRef(cf_inform,pp);
         }
      }
   }

if (expandregex) /* Expand one regex link and hand down */
   {
   char nextbuffer[CF_BUFSIZE],nextbufferOrig[CF_BUFSIZE],regex[CF_BUFSIZE];
   const struct dirent *dirp;
   CFDIR *dirh;
   struct Attributes dummyattr = {{0}};

   memset(&dummyattr,0,sizeof(dummyattr));
   memset(regex,0,CF_BUFSIZE);

   strncpy(regex,ip->name,CF_BUFSIZE-1);

   if ((dirh = OpenDirLocal(pbuffer)) == NULL)
      {
      // Could be a dummy directory to be created so this is not an error.
      CfOut(cf_verbose,""," -> Using best-effort expanded (but non-existent) file base path %s\n",wildpath);
      (*fnptr)(wildpath,pp);
      DeleteItemList(path);
      return;
      }
   else
      {
      count = 0;
   
      for (dirp = ReadDir(dirh); dirp != NULL; dirp = ReadDir(dirh))
         {
         if (!ConsiderFile(dirp->d_name,pbuffer,dummyattr,pp))
            {
            continue;
            }
         
         if (!lastnode && !S_ISDIR(statbuf.st_mode))
            {
            Debug("Skipping non-directory %s\n",dirp->d_name);
            continue;
            }
         
         if (FullTextMatch(regex,dirp->d_name))
            {
            Debug("Link %s matched regex %s\n",dirp->d_name,regex);
            }
         else
            {
            continue;
            }
         
         count++;
         
         strncpy(nextbuffer,pbuffer,CF_BUFSIZE-1);
         AddSlash(nextbuffer);
         strcat(nextbuffer,dirp->d_name);
         
         for (ip = remainder; ip != NULL; ip=ip->next)
            {
            AddSlash(nextbuffer);
            strcat(nextbuffer,ip->name);
            }
         
         /* The next level might still contain regexs, so go again as long as expansion is not nullpotent */
         
         if (!lastnode && (strcmp(nextbuffer,wildpath) != 0))
            {
            LocateFilePromiserGroup(nextbuffer,pp,fnptr);
            }
         else
            {
            struct Promise *pcopy;
            CfOut(cf_verbose,""," -> Using expanded file base path %s\n",nextbuffer);

            /* Now need to recompute any back references to get the complete path */

	    snprintf(nextbufferOrig, sizeof(nextbufferOrig), "%s", nextbuffer);
	    MapNameForward(nextbuffer);

            if (!FullTextMatch(pp->promiser,nextbuffer))
               {
               Debug("Error recomputing references for \"%s\" in: %s",pp->promiser,nextbuffer);
               }

            /* If there were back references there could still be match.x vars to expand */

            pcopy = ExpandDeRefPromise(CONTEXTID,pp);            
            (*fnptr)(nextbufferOrig,pcopy);
            DeletePromise(pcopy);
            }
         }
      
      CloseDir(dirh);
      }
   }
else
   {
   CfOut(cf_verbose,""," -> Using file base path %s\n",pbuffer);
   (*fnptr)(pbuffer,pp);
   }

if (count == 0)
   {
   CfOut(cf_verbose,"","No promiser file objects matched as regular expression %s\n",wildpath);

   if (create)
      {
      VerifyFilePromise(pp->promiser,pp);
      }
   }

DeleteItemList(path);
}

/*********************************************************************/

int IsNewerFileTree(char *dir,time_t reftime)

{ const struct dirent *dirp;
  char path[CF_BUFSIZE] = {0};
  struct Attributes dummyattr = {{0}};
  CFDIR *dirh;
  struct stat sb;

// Assumes that race conditions on the file path are unlikely and unimportant

if (lstat(dir,&sb) == -1)
   {
   CfOut(cf_error,"stat"," !! Unable to stat directory %s in IsNewerFileTree",dir);
   // return true to provoke update
   return true;
   }

if (S_ISDIR(sb.st_mode))
   {
   //CfOut(cf_verbose,""," ?? Looking at %s (%ld)",dir,sb.st_mtime-reftime);      
   
   if (sb.st_mtime > reftime)
      {
      CfOut(cf_verbose,""," >> Detected change in %s",dir);      
      return true;
      }
   }
  
if ((dirh = OpenDirLocal(dir)) == NULL)
   {
   CfOut(cf_error,"opendir"," !! Unable to open directory '%s' in IsNewerFileTree",dir);
   return false;
   }
else
   {
   for (dirp = ReadDir(dirh); dirp != NULL; dirp = ReadDir(dirh))
      {
      if (!ConsiderFile(dirp->d_name,dir,dummyattr,NULL))
         {
         continue;
         }
      
      strncpy(path,dir,CF_BUFSIZE-1);

      if (!JoinPath(path,dirp->d_name))
         {
         CfOut(cf_error,"","Internal limit: Buffer ran out of space adding %s to %s in IsNewerFileTree",dir,path);
         CloseDir(dirh);
         return false;
         }

      if (lstat(path,&sb) == -1)
         {
         CfOut(cf_error,"stat"," !! Unable to stat directory %s in IsNewerFileTree",path);
         CloseDir(dirh);
         // return true to provoke update
         return true;
         }

      if (S_ISDIR(sb.st_mode))
         {
         if (sb.st_mtime > reftime)
            {
            CfOut(cf_verbose,""," >> Detected change in %s",path);      
            CloseDir(dirh);
            return true;
            }
         else
            {
            if (IsNewerFileTree(path,reftime))
               {
               CloseDir(dirh);
               return true;
               }
            }
         }
      }   
   }

CloseDir(dirh);
return false;
}

/*********************************************************************/

int IsDir(char *path)
/*
Checks if the object pointed to by path exists and is a directory.
Returns true if so, false otherwise.
*/
{
#ifdef MINGW
return NovaWin_IsDir(path);
#else
struct stat sb;

if (cfstat(path, &sb) != -1)
   {
   if (S_ISDIR(sb.st_mode))
      {
      return true;
      }
   }

return false;
#endif  /* NOT MINGW */
}

/*********************************************************************/

int EmptyString(char *s)

{ char *sp;

for (sp = s; *sp != '\0'; sp++)
   {
   if (!isspace(*sp))
      {
      return false;
      }
   }

return true;
}

/*********************************************************************/

char *JoinPath(char *path, const char *leaf)

{ int len = strlen(leaf);

Chop(path);
AddSlash(path);

if ((strlen(path)+len) > (CF_BUFSIZE - CF_BUFFERMARGIN))
   {
   CfOut(cf_error,"","Internal limit: Buffer ran out of space constructing string. Tried to add %s to %s\n",leaf,path);
   return NULL;
   }

strcat(path,leaf);
return path;
}

/*********************************************************************/

char *JoinSuffix(char *path,char *leaf)

{ int len = strlen(leaf);

Chop(path);
DeleteSlash(path);
      
if ((strlen(path)+len) > (CF_BUFSIZE - CF_BUFFERMARGIN))
   {
   CfOut(cf_error,"","Internal limit: Buffer ran out of space constructing string. Tried to add %s to %s\n",leaf,path);
   return NULL;
   }

strcat(path,leaf);
return path;
}

/*********************************************************************/

int StartJoin(char *path,char *leaf,int bufsize)

{
*path = '\0';
return JoinMargin(path,leaf,NULL,bufsize,CF_BUFFERMARGIN);
}

/*********************************************************************/

static int StartJoinFast(char *path,char *leaf,char **nextFree,int bufsize)

{
*path = '\0';
*nextFree = path;

return JoinMargin(path,leaf,nextFree,bufsize,CF_BUFFERMARGIN);
}

/*********************************************************************/

int Join(char *path, const char *leaf, int bufsize)

{
 return JoinMargin(path,leaf,NULL,bufsize,CF_BUFFERMARGIN);
}

/*********************************************************************/

int JoinSilent(char *path, const char *leaf, int bufsize)
/* Don't warn on buffer limits - just return the value */
{
 int len = strlen(leaf);

 if ((strlen(path)+len) > (bufsize - CF_BUFFERMARGIN))
    {
    return false;
    }
 
 strcat(path,leaf);
 
 return true;
}

/*********************************************************************/

static int JoinFast(char *path,char *leaf,char **nextFree,int bufsize)
/*
 * Faster stringjoin by keeping track of where we last stopped
 */
{
  return JoinMargin(path,leaf,nextFree,bufsize,CF_BUFFERMARGIN);
}


/*********************************************************************/

int EndJoin(char *path,char *leaf,int bufsize)

{
  return JoinMargin(path,leaf,NULL,bufsize,0);
}

/*********************************************************************/

int JoinMargin(char *path, const char *leaf, char **nextFree, int bufsize, int margin)

{ int len = strlen(leaf);

if (margin < 0)
   {
   FatalError("Negative margin in JoinMargin()");
   }

if(nextFree)
   {
   if ((*nextFree - path) + len > (bufsize - margin) )
      {
      CfOut(cf_error,"","Internal limit: Buffer ran out of space constructing string (using nextFree), len = %d > %d.\n",(strlen(path)+len),(bufsize - CF_BUFFERMARGIN));
      return false;
      }
   
   strcpy(*nextFree, leaf);
   *nextFree += len;
   }
else
   {
   if ((strlen(path)+len) > (bufsize - margin))
      {
      CfOut(cf_error,"","Internal limit: Buffer ran out of space constructing string (%d > %d).\n",(strlen(path)+len),(bufsize - CF_BUFFERMARGIN));
      return false;
      }
   
   strcat(path,leaf);
   }

return true;
}

/*********************************************************************/

int IsAbsPath(char *path)

{
if (IsFileSep(*path))
   {
   return true;
   }
else
   {
   return false;
   }
}

/*******************************************************************/

void AddSlash(char *str)

{ char *sp, *sep = FILE_SEPARATOR_STR;
  int f = false ,b = false;

if (str == NULL)
   {
   return;
   }

// add root slash on Unix systems
if(strlen(str) == 0)
  {
  if((VSYSTEMHARDCLASS != mingw) && (VSYSTEMHARDCLASS != cfnt))
    {
    strcpy(str, "/");
    }
  return;
  }

/* Try to see what convention is being used for filenames
   in case this is a cross-system copy from Win/Unix */

for (sp = str; *sp != '\0'; sp++)
   {
   switch (*sp)
      {
      case '/':
          f = true;
          break;
      case '\\':
          b = true;
          break;
      default:
          break;
      }
   }

if (f && !b)
   {
   sep = "/";
   }
else if (b && !f)
   {
   sep = "\\";
   }

if (!IsFileSep(str[strlen(str)-1]))
   {
   strcat(str,sep);
   }
}

/*********************************************************************/

void DeleteSlash(char *str)

{
if ((strlen(str)== 0) || (str == NULL))
   {
   return;
   }

if (strcmp(str,"/") == 0)
   {
   return;
   }
 
if (IsFileSep(str[strlen(str)-1]))
   {
   str[strlen(str)-1] = '\0';
   }
}

/*********************************************************************/

const char *LastFileSeparator(const char *str)

  /* Return pointer to last file separator in string, or NULL if 
     string does not contains any file separtors */

{ const char *sp;

/* Walk through string backwards */
 
sp = str + strlen(str) - 1;

 while (sp >= str) 
   {
   if (IsFileSep(*sp))
      {
      return sp;
      }
   sp--;
   }

return NULL;
}

/*********************************************************************/

int ChopLastNode(char *str)

  /* Chop off trailing node name (possible blank) starting from
     last character and removing up to the first / encountered 
     e.g. /a/b/c -> /a/b
          /a/b/ -> /a/b                                        */
{
char *sp;
int ret;

/* Here cast is necessary and harmless, str is modifiable */
if ((sp = (char *)LastFileSeparator(str)) == NULL)
   {
   ret = false;
   }
else
   {
   *sp = '\0';
   ret = true;
   }

if (strlen(str) == 0)
   {
   AddSlash(str);
   }
 
return ret; 
}

/*********************************************************************/

void CanonifyNameInPlace(char *s)
{
for (; *s != '\0'; s++)
   {
   if (!isalnum(*s) || *s == '.')
      {
      *s = '_';
      }
   }
}

/*********************************************************************/

char *CanonifyName(const char *str)

{ static char buffer[CF_BUFSIZE];
          
strncpy(buffer,str,CF_BUFSIZE);
CanonifyNameInPlace(buffer);
return buffer;
}

/*********************************************************************/

char *CanonifyChar(const char *str,char ch)

{ static char buffer[CF_BUFSIZE];
  char *sp;

strncpy(buffer,str,CF_BUFSIZE-1);

for (sp = buffer; *sp != '\0'; sp++)
   {
   if (*sp == ch)
      {
      *sp = '_';
      }
   }

return buffer;
}

/*********************************************************************/

int CompareCSVName(char *s1,char *s2)

{ char *sp1,*sp2;
  char ch1,ch2;

for (sp1 = s1,sp2 = s2; *sp1 != '\0' || *sp2 != '\0'; sp1++,sp2++)
   {
   ch1 = (*sp1 == ',') ? '_' : *sp1;
   ch2 = (*sp2 == ',') ? '_' : *sp2;

   if (ch1 > ch2)
      {
      return 1;
      }
   else if (ch1 < ch2)
      {
      return -1;
      }
   }

return 0;
}

/*********************************************************************/

const char *ReadLastNode(const char *str)

/* Return the last node of a pathname string  */

{ const char *sp;
  
if ((sp = LastFileSeparator(str)) == NULL)
   {
   return str;
   }
else
   {
   return sp + 1;
   }
}

/*****************************************************************************/

static void DeEscapeFilename(char *in,char *out)

{ char *sp_in,*sp_out = out;

*sp_out = '\0';

for (sp_in = in; *sp_in != '\0'; sp_in++)
   {
   if (*sp_in == '\\' && *(sp_in+1) == ' ')
      {
      sp_in++;
      }

   *sp_out++ = *sp_in;
   }

*sp_out = '\0';
}

/*****************************************************************************/

int DeEscapeQuotedString(const char *from,char *to)

{ char *cp;
const char *sp;
  char start = *from;
  int len = strlen(from);

if (len == 0)
   {
   return 0;
   }

 for (sp=from+1,cp=to; (sp-from) < len; sp++,cp++)
    {
    if ((*sp == start))
       {
       *(cp) = '\0';

       if (*(sp+1) != '\0')
          {
          return (2+(sp - from));
          }

       return 0;
       }

    if (*sp == '\n')
       {
       P.line_no++;
       }

    if (*sp == '\\')
       {
       switch (*(sp+1))
          {
          case '\n':
              P.line_no++;
              sp+=2;
              break;

          case ' ':
              break;
              
          case '\\':
          case '\"':
          case '\'': sp++;
              break;
          }
       }

    *cp = *sp;    
    }
 
 yyerror("Runaway string");
 *(cp) = '\0';
 return 0;
}

/*********************************************************************/

void Chop(char *str) /* remove trailing spaces */

{ int i;
 
if ((str == NULL) || (strlen(str) == 0))
   {
   return;
   }

if (strlen(str) > CF_EXPANDSIZE)
   {
   CfOut(cf_error,"","Chop was called on a string that seemed to have no terminator");
   return;
   }

for (i = strlen(str)-1; i >= 0 && isspace((int)str[i]); i--)
   {
   str[i] = '\0';
   }
}

/*********************************************************************/

void StripTrailingNewline(char *str)

{ char *c = str + strlen(str);

if (c - str > CF_EXPANDSIZE)
   {
   CfOut(cf_error, "", "StripTrailingNewline was called on an overlong string");
   return;
   }

for (; c >= str && (*c == '\0' || *c == '\n'); --c)
   {
   *c = '\0';
   }
}

/*********************************************************************/

int CompressPath(char *dest,char *src)

{ char *sp;
  char node[CF_BUFSIZE];
  int nodelen;
  int rootlen;

Debug("CompressPath(%s,%s)\n",dest,src);

memset(dest,0,CF_BUFSIZE);

rootlen = RootDirLength(src);
strncpy(dest,src,rootlen);

for (sp = src+rootlen; *sp != '\0'; sp++)
   {
   if (IsFileSep(*sp))
      {
      continue;
      }

   for (nodelen = 0; sp[nodelen] != '\0' && !IsFileSep(sp[nodelen]); nodelen++)
      {
      if (nodelen > CF_MAXLINKSIZE)
         {
         CfOut(cf_error,"","Link in path suspiciously large");
         return false;
         }
      }

   strncpy(node,sp,nodelen);
   node[nodelen] = '\0';
   
   sp += nodelen - 1;
   
   if (strcmp(node,".") == 0)
      {
      continue;
      }
   
   if (strcmp(node,"..") == 0)
      {
      if (!ChopLastNode(dest))
         {
         Debug("cfengine: used .. beyond top of filesystem!\n");
         return false;
         }
   
      continue;
      }
   else
      {
      AddSlash(dest);
      }

   if (!JoinPath(dest,node))
      {
      return false;
      }
   }

return true;
}

/*********************************************************************/

bool IsStrIn(const char *str, const char **strs)
{
int i;

for (i = 0; strs[i]; ++i)
   {
   if (strcmp(str, strs[i]) == 0)
      {
      return true;
      }
   }
return false;
}

bool IsStrCaseIn(const char *str, const char **strs)
{
int i;

for (i = 0; strs[i]; ++i)
   {
   if (strcasecmp(str, strs[i]) == 0)
      {
      return true;
      }
   }
return false;
}

/*********************************************************************/

void FreeStringArray(char **strs)

{ int i;

if (strs == NULL)
   {
   return;
   }

for(i = 0; strs[i] != NULL; i++)
   {
   free(strs[i]);
   }

free(strs);
strs = NULL;
}

/*********************************************************************/

int IsAbsoluteFileName(const char *f)

{ int off = 0;

// Check for quoted strings
 
for (off = 0; f[off] == '\"'; off++)
   {
   }
 
#ifdef NT
if (IsFileSep(f[off]) && IsFileSep(f[off+1]))
   {
   return true;
   }

if (isalpha(f[off]) && f[off+1] == ':' && IsFileSep(f[off+2]))
   {
   return true;
   }
#endif
if (f[off] == '/')
   {
   return true;
   }

return false;
}

bool IsFileOutsideDefaultRepository(const char *f)
{
return (*f == '.') || IsAbsoluteFileName(f);
}

/*******************************************************************/

static int UnixRootDirLength(char *f)
{
if (IsFileSep(*f))
   {
   return 1;
   }

return 0;
}

#ifdef NT
static int NTRootDirLength(char *f)
{
int len;
if (IsFileSep(f[0]) && IsFileSep(f[1]))
   {
   /* UNC style path */

   /* Skip over host name */
   for (len=2; !IsFileSep(f[len]); len++)
      {
      if (f[len] == '\0')
         {
         return len;
         }
      }
   
   /* Skip over share name */

   for (len++; !IsFileSep(f[len]); len++)
      {
      if (f[len] == '\0')
         {
         return len;
         }
      }
   
   /* Skip over file separator */
   len++;
   
   return len;
   }

if (isalpha(f[0]) && f[1] == ':')
   {
   if(IsFileSep(f[2]))
     {
     return 3;
     }

   return 2;  
   }

return UnixRootDirLength(f);
}
#endif

int RootDirLength(char *f)

  /* Return length of Initial directory in path - */

{
#ifdef NT
return NTRootDirLength(f);
#else
return UnixRootDirLength(f);
#endif
}


/*********************************************************************/
/* TOOLKIT : String                                                  */
/*********************************************************************/

char ToLower (char ch)

{
if (isupper((int)ch))
   {
   return(ch - 'A' + 'a');
   }
else
   {
   return(ch);
   }
}


/*********************************************************************/

char ToUpper (char ch)

{
if (isdigit((int)ch) || ispunct((int)ch))
   {
   return(ch);
   }

if (isupper((int)ch))
   {
   return(ch);
   }
else
   {
   return(ch - 'a' + 'A');
   }
}

/*********************************************************************/

char *ToUpperStr (char *str)

{ static char buffer[CF_BUFSIZE];
  int i;

memset(buffer,0,CF_BUFSIZE);
  
if (strlen(str) >= CF_BUFSIZE)
   {
   char *tmp;
   tmp = malloc(40+strlen(str));
   sprintf(tmp,"String too long in ToUpperStr: %s",str);
   FatalError(tmp);
   }

for (i = 0;  (str[i] != '\0') && (i < CF_BUFSIZE-1); i++)
   {
   buffer[i] = ToUpper(str[i]);
   }

buffer[i] = '\0';

return buffer;
}

/*********************************************************************/

char *ToLowerStr (char *str)

{ static char buffer[CF_BUFSIZE];
  int i;

memset(buffer,0,CF_BUFSIZE);

if (strlen(str) >= CF_BUFSIZE-1)
   {
   char *tmp;
   tmp = malloc(40+strlen(str));
   snprintf(tmp,CF_BUFSIZE-1,"String too long in ToLowerStr: %s",str);
   FatalError(tmp);
   }

for (i = 0; (str[i] != '\0') && (i < CF_BUFSIZE-1); i++)
   {
   buffer[i] = ToLower(str[i]);
   }

buffer[i] = '\0';

return buffer;
}

/*********************************************************************/

char *Titleize (char *str)

{ static char buffer[CF_BUFSIZE];
  int i;

if (str == NULL)
   {
   return NULL;
   }
  
strcpy(buffer,str);

if (strlen(buffer) > 1)
   {
   for (i = 1; buffer[i] != '\0'; i++)
      {  
      buffer[i] = ToLower(str[i]);
      }
   }

*buffer = ToUpper(*buffer);

return buffer;
}

/*********************************************************************/

int SubStrnCopyChr(char *to,char *from,int len,char sep)

{ char *sp,*sto = to;
  int count = 0;

memset(to,0,len);

if (from == NULL)
   {
   return 0;
   }

if (from && strlen(from) == 0)
   {
   return 0;
   }

for (sp = from; *sp != '\0'; sp++)
   {
   if (count > len-1)
      {
      break;
      }

   if (*sp == '\\' && *(sp+1) == sep)
      {
      *sto++ = *++sp;
      }
   else if (*sp == sep)
      {
      break;          
      }
   else
      {
      *sto++ = *sp;
      }

   count++;
   }

return count;
}

/*********************************************************************/

int CountChar(char *string,char sep)

{ char *sp;
  int count = 0;

if (string == NULL)
   {
   return 0;
   }

if (string && strlen(string) == 0)
   {
   return 0;
   }

for (sp = string; *sp != '\0'; sp++)
   {
   if (*sp == '\\' && *(sp+1) == sep)
      {
      ++sp;
      }
   else if (*sp == sep)
      {
      count++;
      }
   }

return count;
}

/*********************************************************************/

void ReplaceChar(char *in, char *out, int outSz, char from, char to)

/* Replaces all occurences of 'from' to 'to' in preallocated
 * string 'out'. */
{
  int len;
  int i;

memset(out,0,outSz);
len = strlen(in);
  
for(i = 0; (i < len) && (i < outSz - 1); i++)
   {
   if (in[i] == from)
      {
      out[i] = to;
      }
   else
      {
      out[i] = in[i];
      }
   }
}

/*********************************************************************/

void ReplaceTrailingChar(char *str, char from, char to)

/* Replaces any unwanted last char in str. */
{
 int strLen;

 strLen = strlen(str);

 if(strLen == 0)
    {
    return;
    }
 
 if(str[strLen - 1] == from)
    {
    str[strLen - 1] = to;
    }
}

/*********************************************************************/

void ReplaceTrailingStr(char *str, char *from, char to)

/* Replaces any unwanted last chars in str. */
{
 int strLen;
 int fromLen;

 strLen = strlen(str);
 fromLen = strlen(from);

 if(strLen == 0)
    {
    return;
    }

 char *startCmp = str + strLen - fromLen;
 
 if(strcmp(startCmp, from) == 0)
    {
    memset(startCmp, to, fromLen);
    }
}

/*********************************************************************/

int ReplaceStr(char *in, char *out, int outSz, char* from, char *to)

/* Replaces all occurences of strings 'from' to 'to' in preallocated
 * string 'out'. Returns true on success, false otherwise. */
{
  int inSz;
  int outCount;
  int inCount;
  int fromSz, toSz;

memset(out,0,outSz);

inSz = strlen(in);
fromSz = strlen(from);
toSz = strlen(to);

inCount = 0;
outCount = 0;

while((inCount < inSz) && (outCount < outSz))
   {
    if (strncmp(in + inCount, from, fromSz) == 0)
      {
      if(outCount + toSz >= outSz)
	{
	CfOut(cf_error, "", "!! Out of buffer in ReplaceStr");
	return false;
	}

      strcat(out,to);

      inCount += fromSz;
      outCount += toSz;
      }
   else
      {
      out[outCount] = in[inCount];

      inCount++;
      outCount++;
      }
   }

 return true;
}

/*********************************************************************/

#if defined HAVE_PTHREAD_H && (defined HAVE_LIBPTHREAD || defined BUILDTIN_GCC_THREAD)

void *ThreadUniqueName(pthread_t tid)

/* pthread_t is an integer on Unix, but a structure on Windows
 * Finds a unique name for a thread for both platforms. */

{
#ifdef MINGW
return tid.p;  // pointer to thread structure
#else
return (void*)tid;  // index into thread array ?
#endif  /* NOT MINGW */
}
#endif  /* HAVE PTHREAD */
