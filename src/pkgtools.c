/*----------------------------------------------------------------------*\
|* spkg - The Unofficial Slackware Linux Package Manager                *|
|*                                      designed by Ond�ej Jirman, 2005 *|
|*----------------------------------------------------------------------*|
|*          No copy/usage restrictions are imposed on anybody.          *|
\*----------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

#include "pkgname.h"
#include "untgz.h"
#include "pkgdb.h"
#include "sys.h"
#include "taction.h"
#include "pkgtools.h"
#include "sigtrap.h"
#include "message.h"

/* private 
 ************************************************************************/

#define e_set(n, fmt, args...) e_add(e, "pkgtools", __func__, n, fmt, ##args)

#define _safe_breaking_point(label) \
  do { \
    if (sig_break) \
    { \
      e_set(E_BREAK, "terminated by signal"); \
      goto label; \
    } \
  } while(0)

/* public 
 ************************************************************************/

gint pkg_install(const gchar* pkgfile, const struct pkg_options* opts, struct error* e)
{
  g_assert(pkgfile != 0);
  g_assert(opts != 0);
  g_assert(e != 0);

  msg_setup("install", opts->verbosity);

  /* check if file exist and is regular file */
  if (sys_file_type(pkgfile,1) != SYS_REG)
  {
    e_set(E_ERROR|PKG_NOTEX,"package file does not exist (%s)", pkgfile);
    goto err0;
  }

  /* parse package name from the file path */
  gchar *name, *shortname;
  if ((name = parse_pkgname(pkgfile,5)) == 0 
      || (shortname = parse_pkgname(pkgfile,1)) == 0)
  {
    e_set(E_ERROR|PKG_BADNAME,"package name is invalid (%s)", pkgfile);
    goto err0;
  }

  _safe_breaking_point(err1);

  /* check if package is already in the database */  
  struct db_pkg* pkg=0;
  pkg = db_get_pkg(name,0);
  if (pkg)
  {
    e_set(E_ERROR|PKG_EXIST,"package is already installed (%s)", name);
    db_free_pkg(pkg);
    goto err1;
  }
  if (! (e_errno(e) & DB_NOTEX))
  { /* if error was not because of nonexisting package, then terminate */
    e_set(E_ERROR,"internal error (%s)", name);
    goto err1;
  }
  e_clean(e); /* cleanup error object */

  _safe_breaking_point(err1);

  /* open package's tgz archive */
  struct untgz_state* tgz=0;
  tgz = untgz_open(pkgfile, 0, e);
  if (tgz == 0)
  {
    e_set(E_ERROR|PKG_NOTEX,"can't open package file (%s)", pkgfile);
    goto err1;
  }

  _message("package file opened: %s", pkgfile);

  /* init transaction */
  if (ta_initialize(opts->dryrun, e))
  {
    e_set(E_ERROR,"can't initialize transaction");
    goto err2;
  }

  /* alloc package object */
  pkg = db_alloc_pkg(name);
  pkg->location = g_strdup(pkgfile);

  _safe_breaking_point(err3);

  /* for each file in package */
  gboolean has_doinst = 0;
  while (untgz_get_header(tgz) == 0)
  {
    _safe_breaking_point(err3);

    /* check file path */
    if (tgz->f_name[0] == '/' ||
        strstr(tgz->f_name, "/../") ||
        !strncmp(tgz->f_name, "../", 3))
    {
      /* some damned fucker created this package to mess our system */
      e_set(E_ERROR|PKG_CORRUPT,"package contains files with unsecure paths");
      goto err3;
    }

    /* trim ./ */
    if (strcmp(tgz->f_name, "./") && !strncmp(tgz->f_name, "./", 2))
    {
      gchar* tmp = tgz->f_name;
      tgz->f_name = g_strdup(tmp+2);
      g_free(tmp);
    }

    /* check for metadata files */
    if (!strcmp(tgz->f_name, "install/slack-desc"))
    {
      if (tgz->f_size > 1024*16) /* 16K is enough */
      {
        e_set(E_ERROR|PKG_CORRUPT, "slack-desc file is too big (%d kB)", tgz->f_size/1024);
        goto err3;
      }

      gchar *buf, *desc[11] = {0};
      gsize len;
      untgz_write_data(tgz, &buf, &len);
      parse_slackdesc(buf, shortname, desc);
      pkg->desc = gen_slackdesc(shortname, desc);

      /* free description */
      gint i;
      for (i=0;i<11;i++)
      {
        _message("%s", desc[i]);
        g_free(desc[i]);
      }  
      continue;
    }
    else if (!strcmp(tgz->f_name, "install/doinst.sh"))
    {
      if (tgz->f_size > 1024*512) /* 512K is enough for all. :) */
      {
        e_set(E_ERROR|PKG_CORRUPT, "doinst.sh file is too big (%d kB)", tgz->f_size/1024);
        goto err3;
      }

      gchar* fullpath = g_strdup_printf("%s/%s", opts->root, tgz->f_name);
      if (opts->noptsym) /* optimization disabled, just extract */
      {
        if (untgz_write_file(tgz, fullpath))
        {
          g_free(fullpath);
          e_set(E_ERROR|PKG_BADIO,"file extraction failed %s (%s)", tgz->f_name, pkgfile);
          goto err3;
        }
        g_free(fullpath);
        has_doinst = 1;
        continue;
      }

      /* read doinst.sh into buffer */
      gchar* buf;
      gsize len;
      untgz_write_data(tgz, &buf, &len);
      
      /* optimize out symlinks creation from doinst.sh */
      gchar *b, *end, *ln, *n=buf;
      gchar* doinst = 0;
      while(iter_lines(&b, &end, &n, &ln))
      { /* for each line */
        gchar* dir;
        gchar* link;
        gchar* target;
        /* create link line */
        if (parse_createlink(ln, &dir, &link, &target))
        {
          gchar* path = g_strdup_printf("%s/%s", dir, link);
          g_free(dir);
          g_free(link);
          _message("symlink %s -> %s", path, target);
          pkg->files = g_slist_prepend(pkg->files, db_alloc_file(path, target));
          gchar* fullpath = g_strdup_printf("%s/%s", opts->root, path);
          ta_symlink_nothing(fullpath, g_strdup(target));
        }
        /* if this is not 'delete old file' line... */
        else if (!parse_cleanuplink(ln))
        {
          /* ...append it to doinst buffer */
          /*XXX: this is stupid braindead hack */
          gchar* nd;
          if (doinst)
            nd = g_strdup_printf("%s%s\n", doinst, ln);
          else
            nd = g_strdup_printf("%s\n", ln);
          g_free(doinst);
          doinst = nd;          
        }
        g_free(ln);
      }
      /* we always store full doinst.sh in database */
      pkg->doinst = buf;

      /* write stripped down version of doinst.sh to a file */
      if (doinst != 0)
      {
        if (sys_write_buffer_to_file(fullpath, doinst, (gsize)0, e))
        {
          e_set(E_ERROR|PKG_BADIO,"file extraction failed %s (%s)", tgz->f_name, pkgfile);
          g_free(fullpath);
          goto err3;        
        }
        has_doinst = 1;
      }
      
      g_free(fullpath);
      continue;
    }

    /* add file to db */
    pkg->files = g_slist_append(pkg->files, db_alloc_file(g_strdup(tgz->f_name), 0));

    /* following strings can be freed by the ta code, if so you must zero these
       variables after passing them to a ta_* */
    gchar* fullpath = g_strdup_printf("%s/%s", opts->root, tgz->f_name);
    gchar* temppath = g_strdup_printf("%s--###install###", fullpath);
    sys_ftype existing = sys_file_type(fullpath, 0);

    /* error handling in file installation code
     * ----------------------------------------
     * paranoid -
     * cautious - 
     * normal   -
     * brutal   -
     */

    /* preinstall file (installation will be finished by ta_finalize) */
    switch(tgz->f_type)
    {
      case UNTGZ_DIR: /* we have directory */
        if (existing == SYS_DIR)
        {
          _message("#mkdir %s", tgz->f_name);
          /* installed directory already exist */
//          struct stat st;
//          lstat(fullpath,st);          
        }
        else if (existing == SYS_NONE)
        {
          _message("mkdir %s", tgz->f_name);
          if (!opts->dryrun)
            if (untgz_write_file(tgz, fullpath))
              goto extract_failed;
          ta_keep_remove(fullpath, 1);
          fullpath = 0;
        }
        else if (existing == SYS_ERR)
        {
          _warning("stat failed %s", tgz->f_name);
          /*XXX: bug */
        }
        else
        {
          _warning("can't mkdir over ordinary file %s", tgz->f_name);
          /*XXX: bug (ordinary file) */
        }
      break;
      case UNTGZ_SYM: /* wtf?, symlinks are not permitted to be in package */
        _warning("symlink in archive %s", tgz->f_name);
        /* XXX: bug */
      break;
      case UNTGZ_LNK: /* hardlinks are special beasts, most easy solution is to 
        postpone hardlink creation into transaction finalization phase */
      {
        gchar* linkpath = g_strdup_printf("%s/%s", opts->root, tgz->f_link);
        _message("hardlink found %s -> %s (postponed)", tgz->f_name, tgz->f_link);
        ta_link_nothing(fullpath, linkpath);
        fullpath = 0;
      }
      break;
      case UNTGZ_NONE:
        /*XXX: bug */
      break;
      default: /* ordinary file */
        if (existing == SYS_DIR)
        {
          _warning("can't extract file over dir %s", tgz->f_name);
          /* target path is a directory, bad! */
        }
        else if (existing == SYS_NONE)
        {
          _message("extracting %s", tgz->f_name);
          if (!opts->dryrun)
            if (untgz_write_file(tgz, fullpath))
              goto extract_failed;
          ta_keep_remove(fullpath, 0);
          fullpath = temppath = 0;
        }
        else if (existing == SYS_ERR)
        {
          _warning("stat failed %s", tgz->f_name);
          /*XXX: bug */
        }
        else /* file already exist there */
        {
          /*XXX: here we may check file types, etc. */
          _warning("file already exist %s", tgz->f_name);
          if (!opts->dryrun)
            if (untgz_write_file(tgz, temppath))
              goto extract_failed;
          ta_move_remove(temppath, fullpath);
          fullpath = temppath = 0;
        }
    }
    if (0) /* common error handling */
    {
     extract_failed:
      e_set(E_ERROR|PKG_BADIO,"file extraction failed %s (%s)", tgz->f_name, pkgfile);
      goto err3;
    }
    g_free(temppath);
    g_free(fullpath);
  }
  
  /* error occured during extraction */
  if (!e_ok(e))
  {
    e_set(E_ERROR|PKG_CORRUPT,"package is corrupted (%s)", pkgfile);
    goto err3;
  }

  pkg->usize = tgz->usize/1024;
  pkg->csize = tgz->csize/1024;

  /* add package to the database */
  if (!opts->dryrun)
  {
    _message("updating legacy database");
    if (db_legacy_add_pkg(pkg))
    {
      e_set(E_ERROR|PKG_DB,"can't add package to the legacy database");
      goto err3;
    }
    _safe_breaking_point(err4);
    _message("updating spkg database");
    if (db_add_pkg(pkg))
    {
      e_set(E_ERROR|PKG_DB,"can't add package to the database");
      goto err4;
    }
  }

  /* finalize transaction */
  _message("finalizing transaction");
  ta_finalize();

  /* close tgz */
  _message("closing package");
  untgz_close(tgz);
  tgz = 0;

  if (!opts->dryrun && has_doinst)
  {
    gchar* old_cwd = sys_setcwd(opts->root);
    if (old_cwd)
    {
#if 0
      /* run ldconfig */
      _message("running ldconfig");
      if (system("/sbin/ldconfig -r ."))
        _warning("ldconfig failed");
#endif
      /* run doinst sh */
      if (sys_file_type("install/doinst.sh",0) == SYS_REG)
      {
        _message("running doinst.sh");
        if (system(". install/doinst.sh"))
          _warning("doinst.sh failed");
      }
      sys_setcwd(old_cwd);
    }
  }

  _message("finished");

  db_free_pkg(pkg);
  g_free(name);
  g_free(shortname);
  return 0;

 err4:
  db_legacy_rem_pkg(name);
 err3:
  _message("rolling back");
  ta_rollback();
  db_free_pkg(pkg);
 err2:
  _message("closing package");
  untgz_close(tgz);
 err1:
  g_free(name);
  g_free(shortname);
 err0:
  _message("installation terminated");
  return 1;
}

gint pkg_upgrade(const gchar* pkgfile, const struct pkg_options* opts, struct error* e)
{
  g_assert(pkgfile != 0);
  g_assert(opts != 0);
  g_assert(e != 0);
  e_set(E_FATAL,"command is not yet implemented");
  return 1;
}

gint pkg_remove(const gchar* pkgname, const struct pkg_options* opts, struct error* e)
{
  g_assert(pkgname != 0);
  g_assert(opts != 0);
  g_assert(e != 0);
  e_set(E_FATAL,"command is not yet implemented");
  return 1;
}
