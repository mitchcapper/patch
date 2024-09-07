/* patch - a program to apply diffs to original files */

/* Copyright 1989-2024 Free Software Foundation, Inc.
   Copyright 1984-1988 Larry Wall

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include <common.h>
#include <argmatch.h>
#include <closeout.h>
#include <exitfail.h>
#include <getopt.h>
#include <inp.h>
#include <pch.h>
#include <quotearg.h>
#include <util.h>
#include <version.h>
#include <xalloc.h>
#include <gl_linked_list.h>
#include <gl_xlist.h>
#include <safe.h>

#ifdef __SANITIZE_ADDRESS__
# define FREE_BEFORE_EXIT true
#else
# define FREE_BEFORE_EXIT false
#endif

/* See common.h for the declarations of these variables.  */

bool batch;
bool canonicalize_ws;
bool dry_run;
bool follow_symlinks;
bool force;
bool no_strip_trailing_cr;
bool noreverse_flag;
bool posixly_correct;
bool reverse_flag;
bool set_time;
bool set_utc;
bool skip_rest_of_patch;
bool using_plan_a;
char *inname;
char *outfile;
char *patchbuf;
char *revision;
char const *origbase;
char const *origprae;
char const *origsuff;
enum conflict_style conflict_style;
enum diff diff_type;
enum verbosity verbosity;
idx_t patchbufsize;
#ifndef binary_transput
int binary_transput;
#endif
int inerrno;
intmax_t patch_get;
intmax_t strippath;
lin in_offset;
lin last_frozen_line;
lin out_offset;
signed char invc;
struct stat instat;
struct outfile tmped = { .temporary = true };
struct outfile tmpin = { .temporary = true };
struct outfile tmppat = { .temporary = true };
#ifndef debug
unsigned short int debug;
#endif

/* procedures */

static FILE *create_output_file (struct outfile *, int);
static lin locate_hunk (idx_t);
static bool check_line_endings (lin);
static bool apply_hunk (struct outstate *, lin);
static bool patch_match (lin, lin, idx_t, idx_t);
static bool spew_output (struct outstate *, struct stat *);
static intmax_t numeric_string (char const *, bool, char const *);
static void cleanup (void);
static void get_some_switches (void);
static void init_output (struct outstate *);
static FILE *open_outfile (char *);
static void init_reject (char const *);
static void reinitialize_almost_everything (void);
static void remove_if_needed (struct outfile *);
_Noreturn static void usage (FILE *, int);

static void abort_hunk (char const *, bool, bool);
static void abort_hunk_context (bool, bool);
static void abort_hunk_unified (bool, bool);

static void output_file (struct outfile *, const struct stat *, char *,
			 const struct stat *, mode_t, bool);

static void init_files_to_delete (void);
static void init_files_to_output (void);
static void delete_files (void);
static void output_files (struct stat const *, bool);

#ifdef ENABLE_MERGE
static bool merge;
#else
# define merge false
#endif

static enum diff reject_format = NO_DIFF;  /* automatic */
static bool make_backups;
static bool backup_if_mismatch;
static char const *version_control;
static char const *version_control_context;
static bool remove_empty_files;
static bool explicit_inname;
static enum { RO_IGNORE, RO_WARN, RO_FAIL } read_only_behavior = RO_WARN;

/* true if -R was specified on command line.  */
static bool reverse_flag_specified;

static char const *do_defines; /* symbol to patch using ifdef, ifndef, etc. */
static char const if_defined[] = "\n#ifdef ";
static char const not_defined[] = "\n#ifndef ";
static char const else_defined[] = "\n#else\n";
static char const end_defined[] = "\n#endif\n";

static int Argc;
static char **Argv;

static FILE *rejfp;  /* reject file pointer */

static char const *patchname;
static struct outfile outrej;
static struct outfile tmpout = { .temporary = true };
static struct outfile tmprej = { .temporary = true };

static intmax_t maxfuzz = 2;

static char serrbuf[BUFSIZ];

/* Apply a set of diffs as appropriate. */

int
main (int argc, char **argv)
{
    char const *val;
    bool somefailed = false;
    struct outstate outstate;
    struct stat tmpoutst;
    char numbuf[LINENUM_LENGTH_BOUND + 1];
    bool skip_reject_file = false;
    bool apply_empty_patch = false;
    mode_t file_type;
    int outfd = -1;
    bool have_git_diff = false;

    exit_failure = 2;
    set_program_name (argv[0]);
    init_time ();

    setbuf(stderr, serrbuf);
    atexit (close_stdout);

    patchbufsize = 8 * 1024;
    patchbuf = ximalloc (patchbufsize);

    strippath = -1;

    val = getenv ("QUOTING_STYLE");
    {
      int i = val ? argmatch (val, quoting_style_args, 0, 0) : -1;
      set_quoting_style (nullptr, i < 0 ? shell_quoting_style : i);
    }

    posixly_correct = getenv ("POSIXLY_CORRECT") != 0;
    backup_if_mismatch = ! posixly_correct;
    patch_get = ((val = getenv ("PATCH_GET"))
		 ? numeric_string (val, true, "PATCH_GET value")
		 : 0);

    val = getenv ("SIMPLE_BACKUP_SUFFIX");
    simple_backup_suffix = val && *val ? val : ".orig";

    if ((version_control = getenv ("PATCH_VERSION_CONTROL")))
      version_control_context = "$PATCH_VERSION_CONTROL";
    else if ((version_control = getenv ("VERSION_CONTROL")))
      version_control_context = "$VERSION_CONTROL";

    init_backup_hash_table ();
    init_files_to_delete ();
    init_files_to_output ();

    /* parse switches */
    Argc = argc;
    Argv = argv;
    get_some_switches();

    /* Make get_date() assume that context diff headers use UTC. */
    if (set_utc)
      setenv ("TZ", "UTC", 1);

    if (make_backups | backup_if_mismatch)
      backup_type = get_version (version_control_context, version_control);

    init_output (&outstate);
    if (outfile)
      outstate.ofp = open_outfile (outfile);

    /* Make sure we clean up in case of disaster.  */
    init_signals ();

    /* When the file to patch is specified on the command line, allow that file
       to lie outside the current working tree.  Still doesn't allow to follow
       symlinks.  */
    if (inname)
      unsafe = true;

    if (inname && outfile)
      {
	/* When an input and an output filename is given and the patch is
	   empty, copy the input file to the output file.  In this case, the
	   input file must be a regular file (i.e., symlinks cannot be copied
	   this way).  */
	apply_empty_patch = true;
	file_type = S_IFREG;
	inerrno = -1;
      }
    for (
	open_patch_file (patchname);
	there_is_another_patch (! (inname || posixly_correct), &file_type)
	  || apply_empty_patch;
	reinitialize_almost_everything(),
	  skip_reject_file = false,
	  apply_empty_patch = false
    ) {					/* for each patch in patch file */
      intmax_t hunk = 0;
      intmax_t failed = 0;
      bool mismatch = false;
      char *outname = nullptr;

      if (skip_rest_of_patch)
	somefailed = true;

      if (have_git_diff != pch_git_diff ())
	{
	  if (have_git_diff)
	    {
	      output_files (nullptr, false);
	      inerrno = -1;
	    }
	  have_git_diff = ! have_git_diff;
	}

      if (rejfp)
	{
	  Fclose (rejfp);
	  rejfp = nullptr;
	}
      if (0 <= outfd)
	{
	  close (outfd);
	  outfd = -1;
	}
      remove_if_needed (&tmprej);
      remove_if_needed (&tmpout);
      remove_if_needed (&tmped);

      if (! skip_rest_of_patch && ! file_type)
	{
	  unsigned int old_mode = pch_mode (  reverse_flag) & S_IFMT;
	  unsigned int new_mode = pch_mode (! reverse_flag) & S_IFMT;
	  say ("File %s: can't change file type from %#o to %#o.\n",
	       quotearg (inname), old_mode, new_mode);
	  skip_rest_of_patch = true;
	  somefailed = true;
	}

      if (! skip_rest_of_patch)
	{
	  if (outfile)
	    outname = outfile;
	  else if (pch_copy () || pch_rename ())
	    outname = pch_name (! reverse_flag);
	  else
	    {
	      if (strchr (inname, '\n'))
		fatal ("input/output file name contains newline");
	      outname = inname;
	    }
	}

      if (pch_git_diff () && ! skip_rest_of_patch)
	{
	  struct stat outstat;
	  int outerrno = 0;

	  /* Try to recognize concatenated git diffs based on the SHA1 hashes
	     in the headers.  Will not always succeed for patches that rename
	     or copy files.  */

	  if (! strcmp (inname, outname))
	    {
	      if (inerrno < 0)
		inerrno = stat_file (inname, &instat);
	      outstat = instat;
	      outerrno = inerrno;
	    }
	  else
	    outerrno = stat_file (outname, &outstat);

	  if (! outerrno)
	    {
	      if (has_queued_output (&outstat))
		{
		  output_files (&outstat, false);
		  outerrno = stat_file (outname, &outstat);
		  inerrno = -1;
		}
	      if (! outerrno)
		set_queued_output (&outstat, true);
	    }
	}

      if (! skip_rest_of_patch)
	{
	  if (! get_input_file (inname, outname, file_type))
	    {
	      skip_rest_of_patch = true;
	      somefailed = true;
	    }
	}

      if (read_only_behavior != RO_IGNORE
	  && ! inerrno && ! S_ISLNK (instat.st_mode)
	  && safe_access (inname, W_OK) != 0)
	{
	  say ("File %s is read-only; ", quotearg (inname));
	  if (read_only_behavior == RO_WARN)
	    say ("trying to patch anyway\n");
	  else
	    {
	      say ("refusing to patch\n");
	      skip_rest_of_patch = true;
	      somefailed = true;
	    }
	}

      tmpoutst.st_size = -1;
      outfd = make_tempfile (&tmpout, 'o', outname,
			     O_WRONLY | binary_transput,
			     instat.st_mode & S_IRWXUGO);
      if (outfd < 0)
	{
	  if (errno == ELOOP || errno == EXDEV)
	    {
	      say ("Invalid file name %s -- skipping patch\n", quotearg (outname));
	      skip_rest_of_patch = true;
	      skip_reject_file = true;
	      somefailed = true;
	    }
	  else
	    pfatal ("Can't create temporary file %s", tmpout.name);
	}
      if (diff_type == ED_DIFF) {
	outstate.zero_output = false;
	somefailed |= skip_rest_of_patch;
	do_ed_script (inname, &tmpout, outstate.ofp);
	if (! dry_run && ! outfile && ! skip_rest_of_patch)
	  {
	    if (fstat (outfd, &tmpoutst) != 0)
	      pfatal ("%s", tmpout.name);
	    outstate.zero_output = tmpoutst.st_size == 0;
	  }
	close (outfd);
	outfd = -1;
      } else {
	signed char got_hunk;
	bool apply_anyway = merge;  /* don't try to reverse when merging */

	if (! skip_rest_of_patch && diff_type == GIT_BINARY_DIFF) {
	  say ("File %s: git binary diffs are not supported.\n",
	       quotearg (outname));
	  skip_rest_of_patch = true;
	  somefailed = true;
	}
	/* initialize the patched file */
	if (! skip_rest_of_patch && ! outfile)
	  {
	    init_output (&outstate);
	    outstate.ofp = fdopen (outfd, binary_transput ? "wb" : "w");
	    if (! outstate.ofp)
	      pfatal ("%s", tmpout.name);
	    /* outstate.ofp now owns the file descriptor */
	    outfd = -1;
	  }
	else
	  {
	    /* When writing to a single output file (-o FILE), always pretend
	       that the output file ends in a newline.  Otherwise, when another
	       file is written to the same output file, apply_hunk will fail.  */
	    outstate.after_newline = true;
	  }

	/* find out where all the lines are */
	if (!skip_rest_of_patch) {
	    scan_input (inname, file_type);

	    if (verbosity != SILENT)
	      {
		bool renamed = strcmp (inname, outname);
		bool skip_rename = ! renamed && pch_rename ();

		say ("%s %s %s%c",
		     dry_run ? "checking" : "patching",
		     S_ISLNK (file_type) ? "symbolic link" : "file",
		     quotearg (outname), renamed || skip_rename ? ' ' : '\n');
		if (renamed || skip_rename)
		  say ("(%s%s from %s)\n",
		       skip_rename ? "already " : "",
		       pch_copy () ? "copied" :
		       (pch_rename () ? "renamed" : "read"),
		       ! skip_rename ? inname : pch_name (! strcmp (inname, pch_name (OLD))));
		if (verbosity == VERBOSE)
		  say ("Using Plan %s...\n", using_plan_a ? "A" : "B");
	      }
	}

	/* from here on, open no standard i/o files, because malloc */
	/* might misfire and we can't catch it easily */

	/* apply each hunk of patch */
	while (0 < (got_hunk = another_hunk (diff_type, reverse_flag)))
	  {
	    lin where = 0; /* Pacify 'gcc -Wall'.  */
	    lin newwhere;
	    idx_t fuzz = 0;
	    idx_t mymaxfuzz;

	    if (merge)
	      {
		/* When in merge mode, don't apply with fuzz.  */
		mymaxfuzz = 0;
	      }
	    else
	      {
		idx_t prefix_context = pch_prefix_context ();
		idx_t suffix_context = pch_suffix_context ();
		idx_t context = MAX (prefix_context, suffix_context);
		mymaxfuzz = MIN (maxfuzz, context);
	      }

	    hunk++;
	    if (!skip_rest_of_patch) {
		bool incr_fuzz;
		do {
		    incr_fuzz = true;
		    where = locate_hunk(fuzz);
		    if (! where || fuzz || in_offset)
		      mismatch = true;
		    if (hunk == 1 && ! where && ! (force | apply_anyway)
			&& reverse_flag == reverse_flag_specified) {
						/* dwim for reversed patch? */
			pch_swap ();
			/* Try again.  */
			where = locate_hunk (fuzz);
			if (where
			    && (ok_to_reverse
				("%s patch detected!",
				 (reverse_flag
				  ? "Unreversed"
				  : "Reversed (or previously applied)"))))
			  reverse_flag = ! reverse_flag;
			else
			  {
			    /* Put it back to normal.  */
			    pch_swap ();
			    if (where)
			      {
				apply_anyway = true;
				incr_fuzz = false;
				where = 0;
			      }
			  }
		    }
		} while (!skip_rest_of_patch && !where
			 && (fuzz += incr_fuzz) <= mymaxfuzz);

		if (skip_rest_of_patch) {		/* just got decided */
		  if (outstate.ofp && ! outfile)
		    {
		      Fclose (outstate.ofp);
		      outstate.ofp = nullptr;
		    }
		}
	    }

	    newwhere = (where ? where : pch_first()) + out_offset;
	    if (skip_rest_of_patch
		|| (merge && ! merge_hunk (hunk, &outstate, where,
					   &somefailed))
		|| (! merge
		    && ((where == 1 && pch_says_nonexistent (reverse_flag) == 2
			 && instat.st_size)
			|| ! where
			|| ! apply_hunk (&outstate, where))))
	      {
		if (! skip_reject_file)
		  abort_hunk (outname, ! failed, reverse_flag);
		failed++;
		if (verbosity == VERBOSE ||
		    (! skip_rest_of_patch && verbosity != SILENT))
		  say ("Hunk #%jd %s at %s%s.\n", hunk,
		       skip_rest_of_patch ? "ignored" : "FAILED",
		       format_linenum (numbuf, newwhere),
		       ! skip_rest_of_patch && check_line_endings (newwhere)
			 ?  " (different line endings)" : "");
	      }
	    else if (! merge &&
		     (verbosity == VERBOSE
		      || (verbosity != SILENT && (fuzz || in_offset))))
	      {
		say ("Hunk #%jd succeeded at %s", hunk,
		     format_linenum (numbuf, newwhere));
		if (fuzz)
		  say (" with fuzz %s", format_linenum (numbuf, fuzz));
		if (in_offset)
		  say (" (offset %s line%s)",
		       format_linenum (numbuf, in_offset),
		       &"s"[in_offset == 1]);
		say (".\n");
	      }
	  }

	if (!skip_rest_of_patch)
	  {
	    if (got_hunk < 0  &&  using_plan_a)
	      {
		if (outfile)
		  fatal ("out of memory using Plan A");
		say ("\n\nRan out of memory using Plan A -- trying again...\n\n");
		if (outstate.ofp)
		  {
		    Fclose (outstate.ofp);
		    outstate.ofp = nullptr;
		  }
		continue;
	      }

	    /* Finish spewing out the new file.  */
	    if (! spew_output (&outstate, &tmpoutst))
	      {
		say ("Skipping patch.\n");
		skip_rest_of_patch = true;
	      }
	  }
      }

      /* Block signals because fatal_exit would otherwise have
	 undefined behavior when called from a signal handler that was
	 invoked while the following code was being run.
	 FIXME: The following code does an unbounded amount of work
	 while signals are blocked, which is a bad thing.  */
      if (!dry_run)
	block_signals ();

      /* and put the output where desired */
      if (! skip_rest_of_patch && ! outfile) {
	  bool backup = make_backups
			|| (backup_if_mismatch && (mismatch | failed));
	  if (outstate.zero_output
	      && (remove_empty_files
		  || (pch_says_nonexistent (! reverse_flag) == 2
		      && ! posixly_correct)
		  || S_ISLNK (file_type)))
	    {
	      if (! dry_run)
		output_file (nullptr, nullptr, outname,
			     (inname == outname) ? &instat : nullptr,
			     file_type | 0, backup);
	    }
	  else
	    {
	      if (! outstate.zero_output
		  && pch_says_nonexistent (! reverse_flag) == 2
		  && (remove_empty_files || ! posixly_correct)
		  && ! (merge && somefailed))
		{
		  mismatch = true;
		  somefailed = true;
		  if (verbosity != SILENT)
		    say ("Not deleting file %s as content differs from patch\n",
			 quotearg (outname));
		}

	      if (! dry_run)
		{
		  mode_t old_mode = pch_mode (reverse_flag);
		  mode_t new_mode = pch_mode (! reverse_flag);
		  bool set_mode = new_mode && old_mode != new_mode;

		  /* Avoid replacing files when nothing has changed.  */
		  if (failed < hunk || diff_type == ED_DIFF || set_mode
		      || pch_copy () || pch_rename ())
		    {
		      enum file_attributes attr = 0;
		      struct timespec new_time = p_timestamp[! reverse_flag];
		      mode_t mode = file_type |
			  ((set_mode ? new_mode : instat.st_mode) & S_IRWXUGO);

		      if ((set_time | set_utc) && new_time.tv_sec != -1)
			{
			  struct timespec old_time = p_timestamp[reverse_flag];

			  if (! force && ! inerrno
			      && pch_says_nonexistent (reverse_flag) != 2
			      && old_time.tv_sec != -1
			      && timespec_cmp (old_time,
					       get_stat_mtime (&instat)))
			    say ("Not setting time of file %s "
				 "(time mismatch)\n",
				 quotearg (outname));
			  else if (! force && (mismatch | failed))
			    say ("Not setting time of file %s "
				 "(contents mismatch)\n",
				 quotearg (outname));
			  else
			    attr |= FA_TIMES;
			}

		      if (inerrno)
		        {
			  if (set_mode)
			    attr |= FA_MODE;
			  set_file_attributes (tmpout.name, -1, attr,
					       nullptr, -1, nullptr,
					       mode, &new_time);
			}
		      else
			{
			  attr |= FA_IDS | FA_MODE | FA_XATTRS;
			  set_file_attributes (tmpout.name, -1, attr,
					       inname, -1, &instat,
					       mode, &new_time);
			}

		      output_file (&tmpout,
				   &tmpoutst, outname, nullptr, mode, backup);

		      if (pch_rename ())
			output_file (nullptr, nullptr, inname, &instat,
				     mode, backup);
		    }
		  else if (backup)
		    {
		      struct stat outstat;

		      if (stat_file (outname, &outstat) != 0)
			say ("Cannot stat file %s, skipping backup\n", outname);
		      else
			output_file (&(struct outfile) { .name = outname },
				     &outstat, nullptr, nullptr,
				     file_type | 0, true);
		    }
		}
	    }
      }
      if (diff_type != ED_DIFF) {
	struct stat rejst;

	if (failed && ! skip_reject_file) {
	    Fflush (rejfp);
	    if (fstat (fileno (rejfp), &rejst) < 0)
	      write_fatal ();
	    Fclose (rejfp);
	    rejfp = nullptr;
	    somefailed = true;
	    say ("%jd out of %jd hunk%s %s", failed, hunk, &"s"[hunk == 1],
		 skip_rest_of_patch ? "ignored" : "FAILED");
	    char *rejname = outrej.name;
	    if (outname && (! rejname || strcmp (rejname, "-") != 0)) {
		char *rej = rejname;
		if (!rejname) {
		    /* FIXME: This should really be done differently!  */
		    const char *s = simple_backup_suffix;
		    simple_backup_suffix = ".rej";
		    rej = find_backup_file_name (AT_FDCWD, outname, simple_backups);
		    idx_t len = strlen (rej);
		    if (rej[len - 1] == '~')
		      rej[len - 1] = '#';
		    simple_backup_suffix = s;
		}
		if (! dry_run)
		  {
		    say (" -- saving rejects to file %s\n", quotearg (rej));
		    if (rejname)
		      {
			if (!outrej.exists)
			  copy_file (tmprej.name, nullptr, &outrej, nullptr, 0,
				     S_IFREG | 0666, true);
			else
			  append_to_file (tmprej.name, rejname);
		      }
		    else
		      {
			struct stat oldst;
			int olderrno;

			olderrno = stat_file (rej, &oldst);
			if (olderrno && olderrno != ENOENT)
			  write_fatal ();
		        if (! olderrno && lookup_file_id (&oldst) == CREATED)
			  append_to_file (tmprej.name, rej);
			else
			  move_file (&tmprej,
				     &rejst, rej, S_IFREG | 0666, false);
		      }
		  }
		else
		  say ("\n");
		if (!rejname)
		    free (rej);
	    } else
	      say ("\n");
	}
      }
      if (!dry_run)
	unblock_signals ();
    }
    if (outstate.ofp)
      Fclose (outstate.ofp);
    cleanup ();
    output_files (nullptr, true);
    delete_files ();
    if (somefailed)
      exit (1);
    return 0;
}

/* Prepare to find the next patch to do in the patch file. */

static void
reinitialize_almost_everything (void)
{
    re_patch();
    re_input();

    input_lines = 0;
    last_frozen_line = 0;

    if (inname && ! explicit_inname) {
	free (inname);
	inname = 0;
    }

    in_offset = 0;
    out_offset = 0;

    diff_type = NO_DIFF;

    if (revision) {
	free(revision);
	revision = 0;
    }

    reverse_flag = reverse_flag_specified;
    skip_rest_of_patch = false;
}

static char const shortopts[] = "bB:cd:D:eEfF:g:i:l"
#if 0 && defined ENABLE_MERGE
				"m"
#endif
				"nNo:p:r:RstTuvV:x:Y:z:Z";

static struct option const longopts[] =
{
  {"backup", no_argument, nullptr, 'b'},
  {"prefix", required_argument, nullptr, 'B'},
  {"context", no_argument, nullptr, 'c'},
  {"directory", required_argument, nullptr, 'd'},
  {"ifdef", required_argument, nullptr, 'D'},
  {"ed", no_argument, nullptr, 'e'},
  {"remove-empty-files", no_argument, nullptr, 'E'},
  {"force", no_argument, nullptr, 'f'},
  {"fuzz", required_argument, nullptr, 'F'},
  {"get", required_argument, nullptr, 'g'},
  {"input", required_argument, nullptr, 'i'},
  {"ignore-whitespace", no_argument, nullptr, 'l'},
#ifdef ENABLE_MERGE
  {"merge", optional_argument, nullptr, 'm'},
#endif
  {"normal", no_argument, nullptr, 'n'},
  {"forward", no_argument, nullptr, 'N'},
  {"output", required_argument, nullptr, 'o'},
  {"strip", required_argument, nullptr, 'p'},
  {"reject-file", required_argument, nullptr, 'r'},
  {"reverse", no_argument, nullptr, 'R'},
  {"quiet", no_argument, nullptr, 's'},
  {"silent", no_argument, nullptr, 's'},
  {"batch", no_argument, nullptr, 't'},
  {"set-time", no_argument, nullptr, 'T'},
  {"unified", no_argument, nullptr, 'u'},
  {"version", no_argument, nullptr, 'v'},
  {"version-control", required_argument, nullptr, 'V'},
  {"debug", required_argument, nullptr, 'x'},
  {"basename-prefix", required_argument, nullptr, 'Y'},
  {"suffix", required_argument, nullptr, 'z'},
  {"set-utc", no_argument, nullptr, 'Z'},
  {"dry-run", no_argument, nullptr, CHAR_MAX + 1},
  {"verbose", no_argument, nullptr, CHAR_MAX + 2},
  {"binary", no_argument, nullptr, CHAR_MAX + 3},
  {"help", no_argument, nullptr, CHAR_MAX + 4},
  {"backup-if-mismatch", no_argument, nullptr, CHAR_MAX + 5},
  {"no-backup-if-mismatch", no_argument, nullptr, CHAR_MAX + 6},
  {"posix", no_argument, nullptr, CHAR_MAX + 7},
  {"quoting-style", required_argument, nullptr, CHAR_MAX + 8},
  {"reject-format", required_argument, nullptr, CHAR_MAX + 9},
  {"read-only", required_argument, nullptr, CHAR_MAX + 10},
  {"follow-symlinks", no_argument, nullptr, CHAR_MAX + 11},
  {nullptr, no_argument, nullptr, 0}
};

static char const *const option_help[] =
{
"Input options:",
"",
"  -p NUM  --strip=NUM  Strip NUM leading components from file names.",
"  -F LINES  --fuzz LINES  Set the fuzz factor to LINES for inexact matching.",
"  -l  --ignore-whitespace  Ignore white space changes between patch and input.",
"",
"  -c  --context  Interpret the patch as a context difference.",
"  -e  --ed  Interpret the patch as an ed script.",
"  -n  --normal  Interpret the patch as a normal difference.",
"  -u  --unified  Interpret the patch as a unified difference.",
"",
"  -N  --forward  Ignore patches that appear to be reversed or already applied.",
"  -R  --reverse  Assume patches were created with old and new files swapped.",
"",
"  -i PATCHFILE  --input=PATCHFILE  Read patch from PATCHFILE instead of stdin.",
"",
"Output options:",
"",
"  -o FILE  --output=FILE  Output patched files to FILE.",
"  -r FILE  --reject-file=FILE  Output rejects to FILE.",
"",
"  -D NAME  --ifdef=NAME  Make merged if-then-else output using NAME.",
#ifdef ENABLE_MERGE
"  --merge  Merge using conflict markers instead of creating reject files.",
#endif
"  -E  --remove-empty-files  Remove output files that are empty after patching.",
"",
"  -Z  --set-utc  Set times of patched files, assuming diff uses UTC (GMT).",
"  -T  --set-time  Likewise, assuming local time.",
"",
"  --quoting-style=WORD   output file names using quoting style WORD.",
"    Valid WORDs are: literal, shell, shell-always, c, escape.",
"    Default is taken from QUOTING_STYLE env variable, or 'shell' if unset.",
"",
"Backup and version control options:",
"",
"  -b  --backup  Back up the original contents of each file.",
"  --backup-if-mismatch  Back up if the patch does not match exactly.",
"  --no-backup-if-mismatch  Back up mismatches only if otherwise requested.",
"",
"  -V STYLE  --version-control=STYLE  Use STYLE version control.",
"	STYLE is either 'simple', 'numbered', or 'existing'.",
"  -B PREFIX  --prefix=PREFIX  Prepend PREFIX to backup file names.",
"  -Y PREFIX  --basename-prefix=PREFIX  Prepend PREFIX to backup file basenames.",
"  -z SUFFIX  --suffix=SUFFIX  Append SUFFIX to backup file names.",
"",
"  -g NUM  --get=NUM  Get files from RCS etc. if positive; ask if negative.",
"",
"Miscellaneous options:",
"",
"  -t  --batch  Ask no questions; skip bad-Prereq patches; assume reversed.",
"  -f  --force  Like -t, but ignore bad-Prereq patches, and assume unreversed.",
"  -s  --quiet  --silent  Work silently unless an error occurs.",
"  --verbose  Output extra information about the work being done.",
"  --dry-run  Do not actually change any files; just print what would happen.",
"  --posix  Conform to the POSIX standard.",
"",
"  -d DIR  --directory=DIR  Change the working directory to DIR first.",
"  --reject-format=FORMAT  Create 'context' or 'unified' rejects.",
"  --binary  Read and write data in binary mode.",
"  --read-only=BEHAVIOR  How to handle read-only input files: 'ignore' that they",
"                        are read-only, 'warn' (default), or 'fail'.",
"",
"  -v  --version  Output version info.",
"  --help  Output this help.",
"",
"Report bugs to <" PACKAGE_BUGREPORT ">.",
0
};

static void
usage (FILE *stream, int status)
{
  char const * const *p;

  if (status != 0)
    {
      Fprintf (stream, "%s: Try '%s --help' for more information.\n",
	       program_name, Argv[0]);
    }
  else
    {
      Fprintf (stream, "Usage: %s [OPTION]... [ORIGFILE [PATCHFILE]]\n\n",
	       Argv[0]);
      for (p = option_help;  *p;  p++)
	Fprintf (stream, "%s\n", *p);
    }

  exit (status);
}

/* Process a backup file name option argument of type OPTION_TYPE.  */
static char const *
backup_file_name_option (char const *option_type)
{
  if (!*optarg)
    fatal ("backup %s is empty", option_type);
  if (strchr (optarg, '\n'))
    fatal ("backup %s contains newline", option_type);
  return xstrdup (optarg);
}

/* Process switches and filenames.  */

static void
get_some_switches (void)
{
    int optc;

    free (outrej.name);
    outrej.name = nullptr;
    outrej.exists = nullptr;
    if (optind == Argc)
	return;
    while (0 <= (optc = getopt_long (Argc, Argv, shortopts, longopts, nullptr)))
	switch (optc) {
	    case 'b':
		make_backups = true;
		 /* Special hack for backward compatibility with CVS 1.9.
		    If the last 4 args are '-b SUFFIX ORIGFILE PATCHFILE',
		    treat '-b' as if it were '-b -z'.  */
		if (Argc - optind == 3
		    && strcmp (Argv[optind - 1], "-b") == 0
		    && ! (Argv[optind + 0][0] == '-' && Argv[optind + 0][1])
		    && ! (Argv[optind + 1][0] == '-' && Argv[optind + 1][1])
		    && ! (Argv[optind + 2][0] == '-' && Argv[optind + 2][1]))
		  {
		    optarg = Argv[optind++];
		    if (verbosity != SILENT)
		      say ("warning: the '-b %s' option is obsolete; use '-b -z %s' instead\n",
			   optarg, optarg);
		    goto case_z;
		  }
		break;
	    case 'B':
		origprae = backup_file_name_option ("prefix");
		break;
	    case 'c':
		diff_type = CONTEXT_DIFF;
		break;
	    case 'd':
		if (chdir(optarg) < 0)
		  pfatal ("Can't change to directory %s", quotearg (optarg));
		break;
	    case 'D':
		do_defines = xstrdup (optarg);
		break;
	    case 'e':
		diff_type = ED_DIFF;
		break;
	    case 'E':
		remove_empty_files = true;
		break;
	    case 'f':
		force = true;
		break;
	    case 'F':
		maxfuzz = numeric_string (optarg, false, "fuzz factor");
		break;
	    case 'g':
		patch_get = numeric_string (optarg, true, "get option value");
		break;
	    case 'i':
		patchname = xstrdup (optarg);
		break;
	    case 'l':
		canonicalize_ws = true;
		break;
#ifdef ENABLE_MERGE
	    case 'm':
		merge = true;
		if (optarg)
		  {
		    if (! strcmp (optarg, "merge"))
		      conflict_style = MERGE_MERGE;
		    else if (! strcmp (optarg, "diff3"))
		      conflict_style = MERGE_DIFF3;
		    else
		      usage (stderr, 2);
		  }
		else
		  conflict_style = MERGE_MERGE;
		break;
#endif
	    case 'n':
		diff_type = NORMAL_DIFF;
		break;
	    case 'N':
		noreverse_flag = true;
		break;
	    case 'o':
		if (strchr (optarg, '\n'))
		  fatal ("output file name contains newline");
		outfile = xstrdup (optarg);
		break;
	    case 'p':
		strippath = numeric_string (optarg, false, "strip count");
		break;
	    case 'r':
		outrej.name = xstrdup (optarg);
		break;
	    case 'R':
		reverse_flag = true;
		reverse_flag_specified = true;
		break;
	    case 's':
		verbosity = SILENT;
		break;
	    case 't':
		batch = true;
		break;
	    case 'T':
		set_time = true;
		break;
	    case 'u':
		diff_type = UNI_DIFF;
		break;
	    case 'v':
		version();
		exit (0);
		break;
	    case 'V':
		version_control = optarg;
		version_control_context = "--version-control or -V option";
		break;
#if DEBUGGING
	    case 'x':
		debug = numeric_string (optarg, true, "debugging option");
		break;
#endif
	    case 'Y':
		origbase = backup_file_name_option ("basename prefix");
		break;
	    case 'z':
	    case_z:
		origsuff = backup_file_name_option ("suffix");
		break;
	    case 'Z':
		set_utc = true;
		break;
	    case CHAR_MAX + 1:
		dry_run = true;
		break;
	    case CHAR_MAX + 2:
		verbosity = VERBOSE;
		break;
	    case CHAR_MAX + 3:
		no_strip_trailing_cr = true;
#if HAVE_SETMODE_DOS
		binary_transput = O_BINARY;
#endif
		break;
	    case CHAR_MAX + 4:
		usage (stdout, 0);
	    case CHAR_MAX + 5:
		backup_if_mismatch = true;
		break;
	    case CHAR_MAX + 6:
		backup_if_mismatch = false;
		break;
	    case CHAR_MAX + 7:
		posixly_correct = true;
		break;
	    case CHAR_MAX + 8:
		{
		  int i = argmatch (optarg, quoting_style_args, 0, 0);
		  if (i < 0)
		    {
		      invalid_arg ("quoting style", optarg, i);
		      usage (stderr, 2);
		    }
		  set_quoting_style (nullptr, i);
		}
		break;
	    case CHAR_MAX + 9:
		if (strcmp (optarg, "context") == 0)
		  reject_format = NEW_CONTEXT_DIFF;
		else if (strcmp (optarg, "unified") == 0)
		  reject_format = UNI_DIFF;
		else
		  usage (stderr, 2);
		break;
	    case CHAR_MAX + 10:
		if (strcmp (optarg, "ignore") == 0)
		  read_only_behavior = RO_IGNORE;
		else if (strcmp (optarg, "warn") == 0)
		  read_only_behavior = RO_WARN;
		else if (strcmp (optarg, "fail") == 0)
		  read_only_behavior = RO_FAIL;
		else
		  usage (stderr, 2);
		break;
	    case CHAR_MAX + 11:
		follow_symlinks = true;
		break;
	    default:
		usage (stderr, 2);
	}

    /* Process any filename args.  */
    if (optind < Argc)
      {
	inname = xstrdup (Argv[optind++]);
	explicit_inname = true;
	invc = -1;
	if (optind < Argc)
	  {
	    patchname = xstrdup (Argv[optind++]);
	    if (optind < Argc)
	      {
		Fprintf (stderr, "%s: %s: extra operand\n",
			 program_name, quotearg (Argv[optind]));
		usage (stderr, 2);
	      }
	  }
      }
}

/* Handle STRING (possibly negative if NEGATIVE_ALLOWED is nonzero)
   of type ARGTYPE_MSGID by converting it to an integer,
   returning the result.  If the integer does not fit,
   return an extreme value.  */
static intmax_t
numeric_string (char const *string,
		bool negative_allowed,
		char const *argtype_msgid)
{
  intmax_t value = 0;
  char const *p = string;
  bool negative = *p == '-';
  bool overflow = false;

  p += negative || *p == '+';

  do
    {
      if (!c_isdigit (*p))
	fatal ("%s %s is not a number", argtype_msgid, quotearg (string));
      overflow |= ckd_mul (&value, value, 10);
      overflow |= ckd_add (&value, value, negative ? '0' - *p : *p - '0');
    }
  while (*++p);

  if (value < 0 && ! negative_allowed)
    fatal ("%s %s is negative", argtype_msgid, quotearg (string));

  return !overflow ? value : negative ? INTMAX_MIN : INTMAX_MAX;
}

/* Attempt to find the right place to apply this hunk of patch. */

static lin
locate_hunk (idx_t fuzz)
{
    lin first_guess = pch_first () + in_offset;
    lin offset;
    idx_t pat_lines = pch_ptrn_lines ();
    idx_t prefix_context = pch_prefix_context ();
    idx_t suffix_context = pch_suffix_context ();
    idx_t context = MAX (prefix_context, suffix_context);
    ptrdiff_t prefix_fuzz = fuzz + prefix_context - context;
    ptrdiff_t suffix_fuzz = fuzz + suffix_context - context;
    lin max_where = input_lines - (pat_lines - suffix_fuzz) + 1;
    lin min_where = last_frozen_line + 1;
    lin max_pos_offset = max_where - first_guess;
    lin max_neg_offset = first_guess - min_where;
    lin max_offset = MAX(max_pos_offset, max_neg_offset);
    lin min_offset;

    if (!pat_lines)			/* null range matches always */
	return first_guess;

    /* Do not try lines <= 0.  */
    if (first_guess <= max_neg_offset)
	max_neg_offset = first_guess - 1;

    if (prefix_fuzz < 0 && pch_first () <= 1)
      {
	/* Can only match start of file.  */

	if (suffix_fuzz < 0)
	  /* Can only match entire file.  */
	  if (pat_lines != input_lines || prefix_context < last_frozen_line)
	    return 0;

	offset = 1 - first_guess;
	if (last_frozen_line <= prefix_context
	    && offset <= max_pos_offset
	    && patch_match (first_guess, offset, 0, suffix_fuzz))
	  {
	    in_offset += offset;
	    return first_guess + offset;
	  }
	else
	  return 0;
      }
    else if (prefix_fuzz < 0)
      prefix_fuzz = 0;

    if (suffix_fuzz < 0)
      {
	/* Can only match end of file.  */
	offset = first_guess - (input_lines - pat_lines + 1);
	if (offset <= max_neg_offset
	    && patch_match (first_guess, -offset, prefix_fuzz, 0))
	  {
	    in_offset -= offset;
	    return first_guess - offset;
	  }
	else
	  return 0;
      }

    min_offset = max_pos_offset < 0 ? first_guess - max_where
	       : max_neg_offset < 0 ? first_guess - min_where
	       : 0;
    for (offset = min_offset;  offset <= max_offset;  offset++) {
	char numbuf0[LINENUM_LENGTH_BOUND + 1];
	char numbuf1[LINENUM_LENGTH_BOUND + 1];
	if (offset <= max_pos_offset
	    && patch_match (first_guess, offset, prefix_fuzz, suffix_fuzz)) {
	    if (debug & 1)
	      say ("Offset changing from %s to %s\n",
		   format_linenum (numbuf0, in_offset),
		   format_linenum (numbuf1, in_offset + offset));
	    in_offset += offset;
	    return first_guess+offset;
	}
	if (offset <= max_neg_offset
	    && patch_match (first_guess, -offset, prefix_fuzz, suffix_fuzz)) {
	    if (debug & 1)
	      say ("Offset changing from %s to %s\n",
		   format_linenum (numbuf0, in_offset),
		   format_linenum (numbuf1, in_offset - offset));
	    in_offset -= offset;
	    return first_guess-offset;
	}
    }
    return 0;
}

static void
mangled_patch (idx_t old, idx_t new)
{
  char numbuf0[LINENUM_LENGTH_BOUND + 1];
  char numbuf1[LINENUM_LENGTH_BOUND + 1];
  if (debug & 1)
    say ("oldchar = '%c', newchar = '%c'\n",
        pch_char (old), pch_char (new));
  fatal ("Out-of-sync patch, lines %s,%s -- mangled text or line numbers, "
        "maybe?",
        format_linenum (numbuf0, pch_hunk_beg () + old),
        format_linenum (numbuf1, pch_hunk_beg () + new));
}

/* Output a line number range in unified format.  */

static void
print_unidiff_range (FILE *fp, lin start, idx_t count)
{
  char numbuf0[LINENUM_LENGTH_BOUND + 1];
  char numbuf1[LINENUM_LENGTH_BOUND + 1];

  switch (count)
    {
    case 0:
      Fprintf (fp, "%s,0", format_linenum (numbuf0, start - 1));
      break;

    case 1:
      Fprintf (fp, "%s", format_linenum (numbuf0, start));
      break;

    default:
      Fprintf (fp, "%s,%s",
	       format_linenum (numbuf0, start),
	       format_linenum (numbuf1, count));
      break;
    }
}

static void
print_header_line (FILE *fp, const char *tag, bool reverse)
{
  const char *name = pch_name (reverse);
  const char *timestr = pch_timestr (reverse);
  putline (fp, tag, name ? name : "/dev/null", timestr, nullptr);
}

/* Produce unified reject files */

static void
abort_hunk_unified (bool header, bool reverse)
{
  idx_t old = 1;
  idx_t lastline = pch_ptrn_lines ();
  idx_t new = lastline + 1;
  char const *c_function = pch_c_function();

  if (header)
    {
      if (pch_name (INDEX))
	putline (rejfp, "Index: ", pch_name (INDEX), nullptr);
      print_header_line (rejfp, "--- ", reverse);
      print_header_line (rejfp, "+++ ", ! reverse);
    }

  /* Add out_offset to guess the same as the previous successful hunk.  */
  Fputs ("@@ -", rejfp);
  print_unidiff_range (rejfp, pch_first () + out_offset, lastline);
  Fputs (" +", rejfp);
  print_unidiff_range (rejfp, pch_newfirst () + out_offset, pch_repl_lines ());
  putline (rejfp, " @@", c_function, nullptr);

  while (pch_char (new) == '=' || pch_char (new) == '\n')
    new++;

  if (diff_type != UNI_DIFF)
    pch_normalize (UNI_DIFF);

  for (; ; old++, new++)
    {
      for (;  pch_char (old) == '-';  old++)
	{
	  Fputc ('-', rejfp);
	  pch_write_line (old, rejfp);
	}
      for (;  pch_char (new) == '+';  new++)
	{
	  Fputc ('+', rejfp);
	  pch_write_line (new, rejfp);
	}

      if (old > lastline)
	  break;

      if (pch_char (new) != pch_char (old))
	mangled_patch (old, new);

      Fputc (' ', rejfp);
      pch_write_line (old, rejfp);
    }
  if (pch_char (new) != '^')
    mangled_patch (old, new);
}

/* Output the rejected patch in context format.  */

static void
abort_hunk_context (bool header, bool reverse)
{
    idx_t pat_end = pch_end ();
    /* add in out_offset to guess the same as the previous successful hunk */
    lin oldfirst = pch_first() + out_offset;
    lin newfirst = pch_newfirst() + out_offset;
    lin oldlast = oldfirst + pch_ptrn_lines() - 1;
    lin newlast = newfirst + pch_repl_lines() - 1;
    char const *stars   = diff_type < NEW_CONTEXT_DIFF ? ""       : " ****";
    char const *minuses = diff_type < NEW_CONTEXT_DIFF ? " -----" : " ----";
    char const *c_function = pch_c_function();

    if (diff_type == UNI_DIFF)
      pch_normalize (NEW_CONTEXT_DIFF);

    if (header)
      {
	if (pch_name (INDEX))
	  putline (rejfp, "Index: ", pch_name (INDEX), nullptr);
	print_header_line (rejfp, "*** ", reverse);
	print_header_line (rejfp, "--- ", ! reverse);
      }
    putline (rejfp, "***************", c_function, nullptr);

    for (idx_t i = 0; i <= pat_end; i++) {
	char numbuf0[LINENUM_LENGTH_BOUND + 1];
	char numbuf1[LINENUM_LENGTH_BOUND + 1];
	switch (pch_char(i)) {
	case '*':
	    if (oldlast < oldfirst)
		Fprintf (rejfp, "*** 0%s\n", stars);
	    else if (oldlast == oldfirst)
		Fprintf (rejfp, "*** %s%s\n",
			 format_linenum (numbuf0, oldfirst), stars);
	    else
		Fprintf (rejfp, "*** %s,%s%s\n",
			 format_linenum (numbuf0, oldfirst),
			 format_linenum (numbuf1, oldlast), stars);
	    break;
	case '=':
	    if (newlast < newfirst)
		Fprintf (rejfp, "--- 0%s\n", minuses);
	    else if (newlast == newfirst)
		Fprintf (rejfp, "--- %s%s\n",
			 format_linenum (numbuf0, newfirst), minuses);
	    else
		Fprintf (rejfp, "--- %s,%s%s\n",
			 format_linenum (numbuf0, newfirst),
			 format_linenum (numbuf1, newlast), minuses);
	    break;
	case ' ': case '-': case '+': case '!':
	    Fprintf (rejfp, "%c ", pch_char (i));
	    FALLTHROUGH;
	case '\n':
	    pch_write_line (i, rejfp);
	    break;
	default:
	    fatal ("fatal internal error in abort_hunk_context");
	}
	if (ferror (rejfp))
	  write_fatal ();
    }
}

/* Output the rejected hunk.  */

static void
abort_hunk (char const *outname, bool header, bool reverse)
{
  if (!tmprej.exists)
    init_reject (outname);
  if (reject_format == UNI_DIFF
      || (reject_format == NO_DIFF && diff_type == UNI_DIFF))
    abort_hunk_unified (header, reverse);
  else
    abort_hunk_context (header, reverse);
}

/* We found where to apply it (we hope), so do it. */

static bool
apply_hunk (struct outstate *outstate, lin where)
{
    idx_t old = 1;
    idx_t lastline = pch_ptrn_lines ();
    idx_t new = lastline + 1;
    enum {OUTSIDE, IN_IFNDEF, IN_IFDEF, IN_ELSE} def_state = OUTSIDE;
    char const *R_do_defines = do_defines;
    idx_t pat_end = pch_end ();
    FILE *fp = outstate->ofp;

    where--;
    while (pch_char(new) == '=' || pch_char(new) == '\n')
	new++;

    while (old <= lastline) {
	if (pch_char(old) == '-') {
	    assert (outstate->after_newline);
	    if (! copy_till (outstate, where + old - 1))
		return false;
	    if (R_do_defines) {
		if (def_state == OUTSIDE) {
		    putline (fp, outstate->after_newline + not_defined,
			     R_do_defines, nullptr);
		    def_state = IN_IFNDEF;
		}
		else if (def_state == IN_IFDEF) {
		    Fputs (outstate->after_newline + else_defined, fp);
		    def_state = IN_ELSE;
		}
		if (ferror (fp))
		  write_fatal ();
		outstate->after_newline = pch_write_line (old, fp);
		outstate->zero_output = false;
	    }
	    last_frozen_line++;
	    old++;
	}
	else if (new > pat_end) {
	    break;
	}
	else if (pch_char(new) == '+') {
	    if (! copy_till (outstate, where + old - 1))
		return false;
	    if (R_do_defines) {
		if (def_state == IN_IFNDEF) {
		    Fputs (outstate->after_newline + else_defined, fp);
		    def_state = IN_ELSE;
		}
		else if (def_state == OUTSIDE) {
		    putline (fp, outstate->after_newline + if_defined,
			     R_do_defines, nullptr);
		    def_state = IN_IFDEF;
		}
		if (ferror (fp))
		  write_fatal ();
	    }
	    outstate->after_newline = pch_write_line (new, fp);
	    outstate->zero_output = false;
	    new++;
	}
	else if (pch_char(new) != pch_char(old))
	  mangled_patch (old, new);
	else if (pch_char(new) == '!') {
	    assert (outstate->after_newline);
	    if (! copy_till (outstate, where + old - 1))
		return false;
	    assert (outstate->after_newline);
	    if (R_do_defines) {
	       putline (fp, 1 + not_defined, R_do_defines, nullptr);
	       if (ferror (fp))
		write_fatal ();
	       def_state = IN_IFNDEF;
	    }

	    do
	      {
		if (R_do_defines) {
		    outstate->after_newline = pch_write_line (old, fp);
		}
		last_frozen_line++;
		old++;
	      }
	    while (pch_char (old) == '!');

	    if (R_do_defines) {
		Fputs (outstate->after_newline + else_defined, fp);
		if (ferror (fp))
		  write_fatal ();
		def_state = IN_ELSE;
	    }

	    do
	      {
		outstate->after_newline = pch_write_line (new, fp);
		new++;
	      }
	    while (pch_char (new) == '!');
	    outstate->zero_output = false;
	}
	else {
	    assert(pch_char(new) == ' ');
	    old++;
	    new++;
	    if (R_do_defines && def_state != OUTSIDE) {
		Fputs (outstate->after_newline + end_defined, fp);
		if (ferror (fp))
		  write_fatal ();
		outstate->after_newline = true;
		def_state = OUTSIDE;
	    }
	}
    }
    if (new <= pat_end && pch_char(new) == '+') {
	if (! copy_till (outstate, where + old - 1))
	    return false;
	if (R_do_defines) {
	    if (def_state == OUTSIDE) {
		putline (fp, outstate->after_newline + if_defined,
			 R_do_defines, nullptr);
		def_state = IN_IFDEF;
	    }
	    else if (def_state == IN_IFNDEF) {
		Fputs (outstate->after_newline + else_defined, fp);
		def_state = IN_ELSE;
	    }
	    if (ferror (fp))
	      write_fatal ();
	    outstate->zero_output = false;
	}

	do
	  {
	    if (!outstate->after_newline)
	      Fputc ('\n', fp);
	    outstate->after_newline = pch_write_line (new, fp);
	    outstate->zero_output = false;
	    new++;
	  }
	while (new <= pat_end && pch_char (new) == '+');
    }
    if (R_do_defines && def_state != OUTSIDE) {
	Fputs (outstate->after_newline + end_defined, fp);
	if (ferror (fp))
	  write_fatal ();
	outstate->after_newline = true;
    }
    out_offset += pch_repl_lines() - pch_ptrn_lines ();
    return true;
}

/* Create an output file.  */

static FILE *
create_output_file (struct outfile *out, int open_flags)
{
  int fd = create_file (out, O_WRONLY | binary_transput | open_flags,
			instat.st_mode, true);
  FILE *f = fdopen (fd, binary_transput ? "wb" : "w");
  if (! f)
    pfatal ("Can't create file %s", quotearg (out->name));
  return f;
}

/* Open the new file. */

static void
init_output (struct outstate *outstate)
{
  outstate->ofp = nullptr;
  outstate->after_newline = true;
  outstate->zero_output = true;
}

/* GCC misunderstands dup2; see
   <https://gcc.gnu.org/bugzilla/show_bug.cgi?id=109839>.  */
#if 13 <= __GNUC__
# pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif

static FILE *
open_outfile (char *name)
{
  if (strcmp (name, "-") != 0)
    return create_output_file (&(struct outfile) { .name = name }, 0);
  else
    {
      int stdout_dup = dup (fileno (stdout));
      if (stdout_dup < 0)
	pfatal ("Failed to duplicate standard output");
      FILE *ofp = fdopen (stdout_dup, "a");
      if (! ofp)
	pfatal ("Failed to duplicate standard output");
      if (dup2 (fileno (stderr), fileno (stdout)) < 0)
	pfatal ("Failed to redirect messages to standard error");
      /* FIXME: Do we need to switch stdout_dup into O_BINARY mode here? */
      return ofp;
    }
}

/* Open a file to put hunks we can't locate. */

static void
init_reject (char const *outname)
{
  int fd;
  fd = make_tempfile (&tmprej, 'r', outname, O_WRONLY | binary_transput, 0666);
  if (fd < 0)
    pfatal ("Can't create temporary file %s", tmprej.name);
  rejfp = fdopen (fd, binary_transput ? "wb" : "w");
  if (! rejfp)
    pfatal ("Can't open stream for file %s", quotearg (tmprej.name));
}

/* Copy input file to output, up to wherever hunk is to be applied. */

bool
copy_till (struct outstate *outstate, lin lastline)
{
    lin R_last_frozen_line = last_frozen_line;
    FILE *fp = outstate->ofp;
    char const *s;

    if (R_last_frozen_line > lastline)
      {
	say ("misordered hunks! output would be garbled\n");
	return false;
      }
    while (R_last_frozen_line < lastline)
      {
	idx_t size;
	s = ifetch (++R_last_frozen_line, false, &size);
	if (size)
	  {
	    if (!outstate->after_newline)
	      Fputc ('\n', fp);
	    Fwrite (s, sizeof *s, size, fp);
	    outstate->after_newline = s[size - 1] == '\n';
	    outstate->zero_output = false;
	  }
      }
    last_frozen_line = R_last_frozen_line;
    return true;
}

/* Finish copying the input file to the output file. */

static bool
spew_output (struct outstate *outstate, struct stat *st)
{
    if (debug & 256)
      {
	char numbuf0[LINENUM_LENGTH_BOUND + 1];
	char numbuf1[LINENUM_LENGTH_BOUND + 1];
	say ("il=%s lfl=%s\n",
	     format_linenum (numbuf0, input_lines),
	     format_linenum (numbuf1, last_frozen_line));
      }

    if (last_frozen_line < input_lines)
      if (! copy_till (outstate, input_lines))
	return false;

    if (outstate->ofp && ! outfile)
      {
	Fflush (outstate->ofp);
	if (fstat (fileno (outstate->ofp), st) < 0)
	  write_fatal ();
	Fclose (outstate->ofp);
	outstate->ofp = 0;
      }

    return true;
}

/* Does the patch pattern match at line base+offset? */

static bool
patch_match (lin base, lin offset, idx_t prefix_fuzz, idx_t suffix_fuzz)
{
    idx_t pat_lines = pch_ptrn_lines () - suffix_fuzz;

    for (idx_t pline = 1 + prefix_fuzz; pline <= pat_lines; pline++) {
	idx_t size;
	char const *p = ifetch (pline - 1 + base + offset, 0 <= offset, &size);
	if (canonicalize_ws) {
	    if (!similar(p, size,
			 pfetch(pline),
			 pch_line_len(pline) ))
		return false;
	}
	else if (size != pch_line_len (pline)
		 || memcmp (p, pfetch (pline), size) != 0)
	    return false;
    }
    return true;
}

/* Check if the line endings in the input file and in the patch differ. */

static bool
check_line_endings (lin where)
{
  char const *p = pfetch (1);
  idx_t size = pch_line_len (1);
  if (! size)
    return false;
  bool patch_crlf = 2 <= size && p[size - 2] == '\r' && p[size - 1] == '\n';

  if (! input_lines)
    return false;
  if (where > input_lines)
    where = input_lines;
  p = ifetch (where, false, &size);
  if (! size)
    return false;
  bool input_crlf = 2 <= size && p[size - 2] == '\r' && p[size - 1] == '\n';

  return patch_crlf != input_crlf;
}

/* Do two lines match with canonicalized white space? */

bool
similar (char const *a, idx_t alen, char const *b, idx_t blen)
{
  /* Ignore presence or absence of trailing newlines.  */
  alen  -=  alen && a[alen - 1] == '\n';
  blen  -=  blen && b[blen - 1] == '\n';

  for (;;)
    {
      if (!blen || c_isblank (*b))
	{
	  while (blen && c_isblank (*b))
	    b++, blen--;
	  if (alen)
	    {
	      if (!c_isblank (*a))
		return false;
	      do a++, alen--;
	      while (alen && c_isblank (*a));
	    }
	  if (!alen || !blen)
	    return alen == blen;
	}
      else if (!alen || *a++ != *b++)
	return false;
      else
	alen--, blen--;
    }
}

/* Deferred deletion of files. */

struct file_to_delete {
  char *name;
  struct stat st;
  bool backup;
};

static gl_list_t files_to_delete;

#if FREE_BEFORE_EXIT
static void
dispose_file_to_delete (void const *elt)
{
  free ((void *) elt);
}
#else
# define dispose_file_to_delete nullptr
#endif

static void
init_files_to_delete (void)
{
  files_to_delete = gl_list_create_empty (GL_LINKED_LIST, nullptr, nullptr,
					  dispose_file_to_delete, true);
}

static void
delete_file_later (char *name, const struct stat *st, bool backup)
{
  struct file_to_delete *file_to_delete;
  struct stat st_tmp;

  if (! st)
    {
      if (stat_file (name, &st_tmp) != 0)
	pfatal ("Can't get file attributes of %s %s", "file", name);
      st = &st_tmp;
    }
  file_to_delete = xmalloc (sizeof *file_to_delete);
  file_to_delete->name = xstrdup (name);
  file_to_delete->st = *st;
  file_to_delete->backup = backup;
  gl_list_add_last (files_to_delete, file_to_delete);
  insert_file_id (st, DELETE_LATER);
}

static void
delete_files (void)
{
  gl_list_iterator_t iter;
  const void *elt;

  iter = gl_list_iterator (files_to_delete);
  while (gl_list_iterator_next (&iter, &elt, nullptr))
    {
      const struct file_to_delete *file_to_delete = elt;

      if (lookup_file_id (&file_to_delete->st) == DELETE_LATER)
	{
	  mode_t mode = file_to_delete->st.st_mode;

	  if (verbosity == VERBOSE)
	    say ("Removing %s %s\n",
		 S_ISLNK (mode) ? "symbolic link" : "file",
		 quotearg (file_to_delete->name));
	  move_file (nullptr, nullptr, file_to_delete->name, mode,
		     file_to_delete->backup);
	  removedirs (file_to_delete->name);
	}
    }
  gl_list_iterator_free (&iter);
}

/* Putting output files into place and removing them. */

struct file_to_output {
  char *from;
  struct stat from_st;
  char *to;
  mode_t mode;
  bool backup;
};

static gl_list_t files_to_output;

static void
output_file_later (struct outfile *from, const struct stat *from_st,
		   char const *to, mode_t mode, bool backup)
{
  struct file_to_output *file_to_output;

  file_to_output = xmalloc (sizeof *file_to_output);
  file_to_output->from = xstrdup (from->name);
  file_to_output->from_st = *from_st;
  file_to_output->to = to ? xstrdup (to) : nullptr;
  file_to_output->mode = mode;
  file_to_output->backup = backup;
  gl_list_add_last (files_to_output, file_to_output);
  from->exists = nullptr;
}

static void
output_file_now (struct outfile *from,
		 const struct stat *from_st, char *to,
		 mode_t mode, bool backup)
{
  if (!to)
    {
      if (backup)
	create_backup (from->name, from_st, true);
    }
  else
    {
      assert (0 <= from_st->st_size);
      move_file (from, from_st, to, mode, backup);
    }
}

static void
output_file (struct outfile *from,
	     const struct stat *from_st, char *to,
	     const struct stat *to_st, mode_t mode, bool backup)
{
  if (!from)
    {
      /* Remember which files should be deleted and only delete them when the
	 entire input to patch has been processed.  This allows to correctly
	 determine for which files backup files have already been created.  */

      delete_file_later (to, to_st, backup);
    }
  else if (pch_git_diff () && pch_says_nonexistent (reverse_flag) != 2)
    {
      /* In git-style diffs, the "before" state of each patch refers to the initial
	 state before modifying any files, input files can be referenced more than
	 once (when creating copies), and output files are modified at most once.
	 However, the input to GNU patch may consist of multiple concatenated
	 git-style diffs, which must be processed separately.  (The same output
	 file may go through multiple revisions.)

	 To implement this, we remember which files to /modify/ instead of
	 modifying the files immediately, but we create /new/ output files
	 immediately.  The new output files serve as markers to detect when a
	 file is modified more than once; this allows to recognize most
	 concatenated git-style diffs.
      */

      output_file_later (from, from_st, to, mode, backup);
    }
  else
    output_file_now (from, from_st, to, mode, backup);
}

static void
dispose_file_to_output (const void *elt)
{
  const struct file_to_output *file_to_output = elt;

  free (file_to_output->from);
  free (file_to_output->to);
}

static void
init_files_to_output (void)
{
  files_to_output = gl_list_create_empty (GL_LINKED_LIST, nullptr, nullptr,
					  dispose_file_to_output, true);
}

static void
gl_list_clear (gl_list_t list)
{
  while (gl_list_size (list) > 0)
    gl_list_remove_at (list, 0);
}

static void
output_files (struct stat const *st, bool exiting)
{
  gl_list_iterator_t iter;
  const void *elt;

  iter = gl_list_iterator (files_to_output);
  while (gl_list_iterator_next (&iter, &elt, nullptr))
    {
      const struct file_to_output *file_to_output = elt;
      struct stat const *from_st = &file_to_output->from_st;
      char *name = file_to_output->from;
      struct outfile from = { .name = name, .exists = volatilize (name) };

      output_file_now (&from, from_st, file_to_output->to,
		       file_to_output->mode, file_to_output->backup);
      if (file_to_output->to)
	{
	  char volatile *exists = from.exists;
	  if (exists)
	    safe_unlink (devolatilize (exists));
	}

      if (st && st->st_dev == from_st->st_dev && st->st_ino == from_st->st_ino)
	{
	  /* Free the list up to here. */
	  for (;;)
	    {
	      const void *elt2 = gl_list_get_at (files_to_output, 0);
	      gl_list_remove_at (files_to_output, 0);
	      if (elt == elt2)
		break;
	    }
	  gl_list_iterator_free (&iter);
	  return;
	}
    }
  if (FREE_BEFORE_EXIT || !exiting)
    {
      gl_list_iterator_free (&iter);
      gl_list_clear (files_to_output);
    }
}

/* Fatal exit with cleanup.  If SIG, this is in response to the signal SIG.  */

void
fatal_exit (int sig)
{
  cleanup ();
  if (sig)
    exit_with_signal (sig);

  output_files (nullptr, true);
  exit (2);
}

static void
remove_if_needed (struct outfile *tmp)
{
  char volatile *exists = tmp->exists;
  if (exists)
    {
      safe_unlink (devolatilize (exists));
      tmp->exists = nullptr;
    }
}

static void
cleanup (void)
{
  remove_if_needed (&tmped);
  remove_if_needed (&tmpin);
  remove_if_needed (&tmpout);
  remove_if_needed (&tmppat);
  remove_if_needed (&tmprej);
}
