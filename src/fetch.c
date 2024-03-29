/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "git2/oid.h"
#include "git2/refs.h"
#include "git2/revwalk.h"
#include "git2/transport.h"

#include "common.h"
#include "remote.h"
#include "refspec.h"
#include "pack.h"
#include "fetch.h"
#include "netops.h"
#include "repository.h"
#include "refs.h"


void print_git_vector(git_vector vec) {
	for(int i = 0; i < vec.length; i++) {
		char oidstr[GIT_OID_HEXSZ + 1];
		git_remote_head *head = vec.contents[i];
		git_oid_tostr(oidstr, sizeof(oidstr), &head->oid);
		fprintf(stderr, "remotes->refs.contents[%d]: %s, local: %d\n", i, oidstr, head->local);
	}
}

static int maybe_want(git_remote *remote, git_remote_head *head, git_odb *odb, git_refspec *tagspec, git_remote_autotag_option_t tagopt)
{
	int match = 0;

	if (!git_reference_is_valid_name(head->name))
		return 0;

	if (tagopt == GIT_REMOTE_DOWNLOAD_TAGS_ALL) {
		/*
		 * If tagopt is --tags, always request tags
		 * in addition to the remote's refspecs
		 */
		if (git_refspec_src_matches(tagspec, head->name))
			match = 1;
	}
	for (int i = 0; i < (&remote->active_refspecs)->length; i++) {
		git_refspec *spec = (&remote->active_refspecs)->contents[(i)];
		fprintf(stderr, "remote->active_refspecs[%d]: string: %s; src, %s; dst: %s\n", i, spec->string, spec->src, spec->dst);
	}
	print_git_vector(remote->active_refspecs);
	fprintf(stderr, "print_git_vector %s: %d\n", __FILE__, __LINE__);
	if (!match && git_remote__matching_refspec(remote, head->name))
		match = 1;

	if (!match)
		return 0;

	/* If we have the object, mark it so we don't ask for it */
	if (git_odb_exists(odb, &head->oid)) {
		head->local = 1;
	}
	else
		remote->need_pack = 1;
	char oidstr[GIT_OID_HEXSZ + 1];
	git_oid_tostr(oidstr, sizeof(oidstr), &head->oid);
	fprintf(stderr, "head: %s, local: %d\n", oidstr, head->local);
	return git_vector_insert(&remote->refs, head);
}

static char * filter_wants_1 (git_remote *remote)
{
	git_remote_head **heads;
	int error = 0;
	size_t i, heads_len;
	git_refspec head;
	char oidstr[GIT_OID_HEXSZ + 1];
	char * buff;
	git_vector_clear(&remote->refs);
	error = git_refspec__parse(&head, "HEAD", true);

	if (remote->active_refspecs.length == 0) {
	  if ((error = git_refspec__parse(&head, "HEAD", true)) < 0){
            fprintf (stderr, "can't parse refspec %s:%d\n", __FILE__, __LINE__);
	    return NULL;
	  }

	  error = git_refspec__dwim_one(&remote->active_refspecs, &head, &remote->refs);
	  git_refspec__free(&head);
	  if (error < 0){
            fprintf (stderr, "git_refspec__dwim_one  %s:%d\n", __FILE__, __LINE__);
	    return NULL;
          }
	}
	if (git_remote_ls ((const git_remote_head ***)&heads, &heads_len, remote) < 0){
          fprintf (stderr, "problem with git_remote_ls %s:%d\n", __FILE__, __LINE__);
	  return NULL;
	}
	//fprintf (stderr, "filter_wants_1 len=%ld %s:%d\n", heads_len, __FILE__, __LINE__);
	buff = malloc ((GIT_OID_HEXSZ + 1)*heads_len);
	for (i = 0; i < heads_len; i++) {
	  git_oid_tostr (oidstr, sizeof(oidstr), &heads[i]->oid);
          //fprintf (stderr, "filter_wants_1 %s %s:%d\n", oidstr, __FILE__, __LINE__);
	  sprintf (buff + (GIT_OID_HEXSZ + 1)*i, "%s", oidstr);
	  if (i>0 && i < heads_len) buff [(GIT_OID_HEXSZ + 1)*i-1] = ';';
	}

	return buff;
}

static int filter_wants(git_remote *remote, const git_fetch_options *opts)
{
	git_remote_head **heads;
	git_refspec tagspec, head;
	int error = 0;
	git_odb *odb;
	size_t i, heads_len;
	git_remote_autotag_option_t tagopt = remote->download_tags;
	char oidstr[GIT_OID_HEXSZ + 1];
	fprintf (stderr, "filter_wants %s:%d\n", __FILE__, __LINE__);

	if (opts && opts->download_tags != GIT_REMOTE_DOWNLOAD_TAGS_UNSPECIFIED)
		tagopt = opts->download_tags;

	print_git_vector(remote->refs);
	fprintf(stderr, "print_git_vector %s: %d\n", __FILE__, __LINE__);
	git_vector_clear(&remote->refs);
	if ((error = git_refspec__parse(&tagspec, GIT_REFSPEC_TAGS, true)) < 0)
		return error;
	print_git_vector(remote->refs);
	fprintf(stderr, "print_git_vector %s: %d\n", __FILE__, __LINE__);
	/*
	 * The fetch refspec can be NULL, and what this means is that the
	 * user didn't specify one. This is fine, as it means that we're
	 * not interested in any particular branch but just the remote's
	 * HEAD, which will be stored in FETCH_HEAD after the fetch.
	 */
	if (remote->active_refspecs.length == 0) {
		if ((error = git_refspec__parse(&head, "HEAD", true)) < 0)
			goto cleanup;

		error = git_refspec__dwim_one(&remote->active_refspecs, &head, &remote->refs);
		git_refspec__free(&head);

		if (error < 0)
			goto cleanup;
	}
	fprintf (stderr, "filter_wants %s:%d\n", __FILE__, __LINE__);
	print_git_vector(remote->refs);
	fprintf(stderr, "print_git_vector %s: %d\n", __FILE__, __LINE__);
	if (git_repository_odb__weakptr(&odb, remote->repo) < 0)
		goto cleanup;

	if (git_remote_ls((const git_remote_head ***)&heads, &heads_len, remote) < 0)
		goto cleanup;
	print_git_vector(remote->refs);
	fprintf(stderr, "print_git_vector %s: %d\n", __FILE__, __LINE__);
	fprintf (stderr, "filter_wants heads=%ld %s:%d\n", heads_len, __FILE__, __LINE__);
	
	for (i = 0; i < heads_len; i++) {
	  git_oid_tostr (oidstr, sizeof(oidstr), &heads[i]->oid);
	  fprintf (stderr, "filter_wants head=%ld local=%d id=%s name=%s %s:%d\n", i, heads[i]->local, oidstr,
		   heads[i]->name, __FILE__, __LINE__);
	  if ((error = maybe_want(remote, heads[i], odb, &tagspec, tagopt)) < 0)
			break;
	}

cleanup:
	git_refspec__free(&tagspec);

	return error;
}

/*
 * In this first version, we push all our refs in and start sending
 * them out. When we get an ACK we hide that commit and continue
 * traversing until we're done
 */
int git_fetch_negotiate(git_remote *remote, const git_fetch_options *opts)
{
	git_transport *t = remote->transport;

	remote->need_pack = 0;

	fprintf (stderr, "git_fetch_negotiate %s:%d\n", __FILE__, __LINE__);

	// Get the commit hash for the remote's HEAD and refs/heads/master
	if (filter_wants(remote, opts) < 0) {
		giterr_set(GITERR_NET, "Failed to filter the reference list for wants");
		return -1;
	}


	/* Don't try to negotiate when we don't want anything */
	if (!remote->need_pack)
		return 0;

	// in case its not here proceed with negotiation
	fprintf (stderr, "git_fetch_negotiate need %ld %s:%d\n", remote->refs.length, __FILE__, __LINE__);

	/*
	 * Now we have everything set up so we can start tell the
	 * server what we want and what we have.
	 */
	print_git_vector(remote->refs);
	fprintf(stderr, "print_git_vector %s: %d\n", __FILE__, __LINE__);
	return t->negotiate_fetch(t,
		remote->repo,
		(const git_remote_head * const *)remote->refs.contents,
		remote->refs.length);
}


int git_get_last (git_remote *remote, char ** out)
{
  int error = -1;
  if (!git_remote_connected(remote) &&
      (error = git_remote_connect(remote, GIT_DIRECTION_FETCH, NULL, NULL, NULL)) < 0){
    fprintf(stderr, "could not connect to remote\n");
    return -1;
  }
  // Get the commit hash for the remote's HEAD and refs/heads/master
  *out = filter_wants_1 (remote);
  return 0;
}



int git_fetch_download_pack(git_remote *remote, const git_remote_callbacks *callbacks)
{
	git_transport *t = remote->transport;
	git_transfer_progress_cb progress = NULL;
	void *payload = NULL;

	fprintf (stderr, "git_fetch_download_pack %s:%d\n", __FILE__, __LINE__);
	if (!remote->need_pack)
		return 0;

	fprintf (stderr, "git_fetch_download_pack need %s:%d\n", __FILE__, __LINE__);
	if (callbacks) {
		progress = callbacks->transfer_progress;
		payload  = callbacks->payload;
	}

	return t->download_pack(t, remote->repo, &remote->stats, progress, payload);
}

int git_fetch_init_options(git_fetch_options *opts, unsigned int version)
{
	GIT_INIT_STRUCTURE_FROM_TEMPLATE(
		opts, version, git_fetch_options, GIT_FETCH_OPTIONS_INIT);
	return 0;
}
