/* -*- c-file-style: "linux" -*-

   Copyright (C) 1996-2000 by Andrew Tridgell
   Copyright (C) Paul Mackerras 1996

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "rsync.h"

extern int verbose;
extern int recurse;
extern int delete_mode;
extern int delete_after;
extern int max_delete;
extern int csum_length;
extern struct stats stats;
extern int dry_run;
extern int am_server;
extern int relative_paths;
extern int keep_dirlinks;
extern int preserve_hard_links;
extern int preserve_perms;
extern int cvs_exclude;
extern int io_error;
extern char *tmpdir;
extern char *compare_dest;
extern int make_backups;
extern int do_progress;
extern char *backup_dir;
extern char *backup_suffix;
extern int backup_suffix_len;
extern int cleanup_got_literal;
extern int module_id;
extern int ignore_errors;
extern int orig_umask;
extern int keep_partial;
extern int checksum_seed;
extern int inplace;

extern struct exclude_list_struct server_exclude_list;


static void delete_one(char *fn, int is_dir)
{
	if (!is_dir) {
		if (robust_unlink(fn) != 0) {
			rsyserr(FERROR, errno, "delete_one: unlink %s failed",
				full_fname(fn));
		} else if (verbose)
			rprintf(FINFO, "deleting %s\n", fn);
	} else {
		if (do_rmdir(fn) != 0) {
			if (errno == ENOTDIR && keep_dirlinks) {
				delete_one(fn, 0);
				return;
			}
			if (errno != ENOTEMPTY && errno != EEXIST) {
				rsyserr(FERROR, errno,
					"delete_one: rmdir %s failed",
					full_fname(fn));
			}
		} else if (verbose)
			rprintf(FINFO, "deleting directory %s\n", fn);
	}
}


static int is_backup_file(char *fn)
{
	int k = strlen(fn) - backup_suffix_len;
	return k > 0 && strcmp(fn+k, backup_suffix) == 0;
}


/* This deletes any files on the receiving side that are not present
 * on the sending side. */
void delete_files(struct file_list *flist)
{
	struct file_list *local_file_list;
	int i, j;
	char *argv[1], fbuf[MAXPATHLEN];
	static int deletion_count;

	if (cvs_exclude)
		add_cvs_excludes();

	if (io_error && !(lp_ignore_errors(module_id) || ignore_errors)) {
		rprintf(FINFO,"IO error encountered - skipping file deletion\n");
		return;
	}

	for (j = 0; j < flist->count; j++) {
		if (!(flist->files[j]->flags & FLAG_TOP_DIR)
		    || !S_ISDIR(flist->files[j]->mode))
			continue;

		argv[0] = f_name_to(flist->files[j], fbuf);

		if (!(local_file_list = send_file_list(-1, 1, argv)))
			continue;

		if (verbose > 1)
			rprintf(FINFO, "deleting in %s\n", fbuf);

		for (i = local_file_list->count-1; i >= 0; i--) {
			if (max_delete && deletion_count > max_delete)
				break;
			if (!local_file_list->files[i]->basename)
				continue;
			if (flist_find(flist,local_file_list->files[i]) < 0) {
				char *f = f_name(local_file_list->files[i]);
				if (make_backups && (backup_dir || !is_backup_file(f))) {
					make_backup(f);
					if (verbose)
						rprintf(FINFO, "deleting %s\n", f);
				} else {
					int mode = local_file_list->files[i]->mode;
					delete_one(f, S_ISDIR(mode) != 0);
				}
				deletion_count++;
			}
		}
		flist_free(local_file_list);
	}
}


/*
 * get_tmpname() - create a tmp filename for a given filename
 *
 *   If a tmpdir is defined, use that as the directory to
 *   put it in.  Otherwise, the tmp filename is in the same
 *   directory as the given name.  Note that there may be no
 *   directory at all in the given name!
 *
 *   The tmp filename is basically the given filename with a
 *   dot prepended, and .XXXXXX appended (for mkstemp() to
 *   put its unique gunk in).  Take care to not exceed
 *   either the MAXPATHLEN or NAME_MAX, esp. the last, as
 *   the basename basically becomes 8 chars longer. In that
 *   case, the original name is shortened sufficiently to
 *   make it all fit.
 *
 *   Of course, there's no real reason for the tmp name to
 *   look like the original, except to satisfy us humans.
 *   As long as it's unique, rsync will work.
 */

static int get_tmpname(char *fnametmp, char *fname)
{
	char *f;
	int     length = 0;
	int	maxname;

	if (tmpdir) {
		/* Note: this can't overflow, so the return value is safe */
		length = strlcpy(fnametmp, tmpdir, MAXPATHLEN - 2);
		fnametmp[length++] = '/';
		fnametmp[length] = '\0';	/* always NULL terminated */
	}

	if ((f = strrchr(fname, '/')) != NULL) {
		++f;
		if (!tmpdir) {
			length = f - fname;
			/* copy up to and including the slash */
			strlcpy(fnametmp, fname, length + 1);
		}
	} else
		f = fname;
	fnametmp[length++] = '.';
	fnametmp[length] = '\0';		/* always NULL terminated */

	maxname = MIN(MAXPATHLEN - 7 - length, NAME_MAX - 8);

	if (maxname < 1) {
		rprintf(FERROR, "temporary filename too long: %s\n", fname);
		fnametmp[0] = '\0';
		return 0;
	}

	strlcpy(fnametmp + length, f, maxname);
	strcat(fnametmp + length, ".XXXXXX");

	return 1;
}


static int receive_data(int f_in,struct map_struct *mapbuf,int fd,char *fname,
			OFF_T total_size)
{
	int i;
	struct sum_struct sum;
	unsigned int len;
	OFF_T offset = 0;
	OFF_T offset2;
	char *data;
	static char file_sum1[MD4_SUM_LENGTH];
	static char file_sum2[MD4_SUM_LENGTH];
	char *map = NULL;

	read_sum_head(f_in, &sum);

	sum_init(checksum_seed);

	while ((i = recv_token(f_in, &data)) != 0) {
		if (do_progress)
			show_progress(offset, total_size);

		if (i > 0) {
			if (verbose > 3) {
				rprintf(FINFO,"data recv %d at %.0f\n",
					i,(double)offset);
			}

			stats.literal_data += i;
			cleanup_got_literal = 1;

			sum_update(data,i);

			if (fd != -1 && write_file(fd,data,i) != i) {
				rsyserr(FERROR, errno, "write failed on %s",
					full_fname(fname));
				exit_cleanup(RERR_FILEIO);
			}
			offset += i;
			continue;
		}

		i = -(i+1);
		offset2 = i*(OFF_T)sum.blength;
		len = sum.blength;
		if (i == (int)sum.count-1 && sum.remainder != 0)
			len = sum.remainder;

		stats.matched_data += len;

		if (verbose > 3)
			rprintf(FINFO,"chunk[%d] of size %d at %.0f offset=%.0f\n",
				i,len,(double)offset2,(double)offset);

		if (mapbuf) {
			map = map_ptr(mapbuf,offset2,len);

			see_token(map, len);
			sum_update(map,len);
		}

		if (!inplace || offset != offset2) {
			if (fd != -1 && write_file(fd, map, len) != (int)len) {
				rsyserr(FERROR, errno, "write failed on %s",
					full_fname(fname));
				exit_cleanup(RERR_FILEIO);
			}
		} else {
			flush_write_file(fd);
			if (do_lseek(fd,(OFF_T)len,SEEK_CUR) != offset+len) {
				rprintf(FERROR, "lseek failed on %s: %s, %lli, %lli, %i\n",
					full_fname(fname), strerror(errno), do_lseek(fd,0,SEEK_CUR), (offset+len), i);
				exit_cleanup(RERR_FILEIO);
			}
		}
		offset += len;
	}

	flush_write_file(fd);

#ifdef HAVE_FTRUNCATE
	if (inplace)
		ftruncate(fd, offset);
#endif

	if (do_progress)
		end_progress(total_size);

	if (fd != -1 && offset > 0 && sparse_end(fd) != 0) {
		rsyserr(FERROR, errno, "write failed on %s",
			full_fname(fname));
		exit_cleanup(RERR_FILEIO);
	}

	sum_end(file_sum1);

	read_buf(f_in,file_sum2,MD4_SUM_LENGTH);
	if (verbose > 2)
		rprintf(FINFO,"got file_sum\n");
	if (fd != -1 && memcmp(file_sum1, file_sum2, MD4_SUM_LENGTH) != 0)
		return 0;
	return 1;
}


static void discard_receive_data(int f_in, OFF_T length)
{
	receive_data(f_in, NULL, -1, NULL, length);
}


/**
 * main routine for receiver process.
 *
 * Receiver process runs on the same host as the generator process. */
int recv_files(int f_in, struct file_list *flist, char *local_name)
{
	int fd1,fd2;
	STRUCT_STAT st;
	char *fname, fbuf[MAXPATHLEN];
	char template[MAXPATHLEN];
	char fnametmp[MAXPATHLEN];
	char *fnamecmp;
	char fnamecmpbuf[MAXPATHLEN];
	struct map_struct *mapbuf;
	struct file_struct *file;
	struct stats initial_stats;
	int save_make_backups = make_backups;
	int i, recv_ok, phase = 0;

	if (verbose > 2)
		rprintf(FINFO,"recv_files(%d) starting\n",flist->count);

	if (flist->hlink_pool) {
		pool_destroy(flist->hlink_pool);
		flist->hlink_pool = NULL;
	}

	while (1) {
		cleanup_disable();

		i = read_int(f_in);
		if (i == -1) {
			if (phase)
				break;

			phase = 1;
			csum_length = SUM_LENGTH;
			if (verbose > 2)
				rprintf(FINFO, "recv_files phase=%d\n", phase);
			send_msg(MSG_DONE, "", 0);
			if (keep_partial)
				make_backups = 0; /* prevents double backup */
			continue;
		}

		if (i < 0 || i >= flist->count) {
			rprintf(FERROR,"Invalid file index %d in recv_files (count=%d)\n",
				i, flist->count);
			exit_cleanup(RERR_PROTOCOL);
		}

		file = flist->files[i];

		stats.current_file_index = i;
		stats.num_transferred_files++;
		stats.total_transferred_size += file->length;
		cleanup_got_literal = 0;

		if (local_name)
			fname = local_name;
		else
			fname = f_name_to(file, fbuf);

		if (dry_run) {
			if (!am_server && verbose)
				rprintf(FINFO, "%s\n", fname);
			continue;
		}

		initial_stats = stats;

		if (verbose > 2)
			rprintf(FINFO,"recv_files(%s)\n",fname);

		fnamecmp = fname;

		if (server_exclude_list.head
		    && check_exclude(&server_exclude_list, fname,
				     S_ISDIR(file->mode)) < 0) {
			if (verbose) {
				rprintf(FINFO,
					"skipping server-excluded update for \"%s\"\n",
					fname);
			}
			discard_receive_data(f_in, file->length);
			continue;
		}

		/* open the file */
		fd1 = do_open(fnamecmp, O_RDONLY, 0);

		if (fd1 == -1 && compare_dest != NULL) {
			/* try the file at compare_dest instead */
			pathjoin(fnamecmpbuf, sizeof fnamecmpbuf,
				 compare_dest, fname);
			fnamecmp = fnamecmpbuf;
			fd1 = do_open(fnamecmp, O_RDONLY, 0);
		}

		if (fd1 != -1 && do_fstat(fd1,&st) != 0) {
			rsyserr(FERROR, errno, "fstat %s failed",
				full_fname(fnamecmp));
			discard_receive_data(f_in, file->length);
			close(fd1);
			continue;
		}

		if (fd1 != -1 && S_ISDIR(st.st_mode) && fnamecmp == fname) {
			/* this special handling for directories
			 * wouldn't be necessary if robust_rename()
			 * and the underlying robust_unlink could cope
			 * with directories
			 */
			rprintf(FERROR,"recv_files: %s is a directory\n",
				full_fname(fnamecmp));
			discard_receive_data(f_in, file->length);
			close(fd1);
			continue;
		}

		if (fd1 != -1 && !S_ISREG(st.st_mode)) {
			close(fd1);
			fd1 = -1;
			mapbuf = NULL;
		}

		if (fd1 != -1 && !preserve_perms) {
			/* if the file exists already and we aren't preserving
			 * permissions then act as though the remote end sent
			 * us the file permissions we already have */
			file->mode = st.st_mode;
		}

		if (fd1 != -1 && st.st_size > 0) {
			mapbuf = map_file(fd1,st.st_size);
			if (verbose > 2) {
				rprintf(FINFO, "recv mapped %s of size %.0f\n",
					fnamecmp, (double)st.st_size);
			}
		} else
			mapbuf = NULL;

		/* We now check to see if we are writing file "inplace" */
		if (inplace)  {
			fd2 = do_open(fnamecmp, O_WRONLY|O_CREAT, 0);
			if (fd2 == -1) {
				rsyserr(FERROR, errno, "open %s failed",
					full_fname(fnamecmp));
				discard_receive_data(f_in, file->length);
				if (mapbuf)
					unmap_file(mapbuf);
				if (fd1 != -1)
					close(fd1);
				continue;
			}
		} else {
			if (!get_tmpname(fnametmp,fname)) {
				discard_receive_data(f_in, file->length);
				if (mapbuf)
					unmap_file(mapbuf);
				if (fd1 != -1)
					close(fd1);
				continue;
			}

			strlcpy(template, fnametmp, sizeof template);

			/* we initially set the perms without the
			 * setuid/setgid bits to ensure that there is no race
			 * condition. They are then correctly updated after
			 * the lchown. Thanks to snabb@epipe.fi for pointing
			 * this out.  We also set it initially without group
			 * access because of a similar race condition. */
			fd2 = do_mkstemp(fnametmp, file->mode & INITACCESSPERMS);

			/* in most cases parent directories will already exist
			 * because their information should have been previously
			 * transferred, but that may not be the case with -R */
			if (fd2 == -1 && relative_paths && errno == ENOENT
			    && create_directory_path(fnametmp, orig_umask) == 0) {
				strlcpy(fnametmp, template, sizeof fnametmp);
				fd2 = do_mkstemp(fnametmp, file->mode & INITACCESSPERMS);
			}
			if (fd2 == -1) {
				rsyserr(FERROR, errno, "mkstemp %s failed",
					full_fname(fnametmp));
				discard_receive_data(f_in, file->length);
				if (mapbuf)
					unmap_file(mapbuf);
				if (fd1 != -1)
					close(fd1);
				continue;
			}

			cleanup_set(fnametmp, fname, file, mapbuf, fd1, fd2);
		}

		if (!am_server && verbose)
			rprintf(FINFO, "%s\n", fname);

		/* recv file data */
		recv_ok = receive_data(f_in,mapbuf,fd2,fname,file->length);

		log_recv(file, &initial_stats);

		if (mapbuf)
			unmap_file(mapbuf);
		if (fd1 != -1)
			close(fd1);
		if (close(fd2) < 0) {
			rsyserr(FERROR, errno, "close failed on %s",
				full_fname(fnametmp));
			exit_cleanup(RERR_FILEIO);
		}

		if (recv_ok || keep_partial)
			finish_transfer(fname, fnametmp, file, recv_ok);
		else
			do_unlink(fnametmp);

		cleanup_disable();

		if (!recv_ok) {
			if (csum_length == SUM_LENGTH) {
				rprintf(FERROR,"ERROR: file corruption in %s. File changed during transfer?\n",
					full_fname(fname));
			} else {
				char buf[4];
				if (verbose > 1)
					rprintf(FINFO,"redoing %s(%d)\n",fname,i);
				SIVAL(buf, 0, i);
				send_msg(MSG_REDO, buf, 4);
			}
		}
	}
	make_backups = save_make_backups;

	if (delete_after && recurse && delete_mode && !local_name
	    && flist->count > 0)
		delete_files(flist);

	if (verbose > 2)
		rprintf(FINFO,"recv_files finished\n");

	return 0;
}
