/*
 *  gitLink
 *
 *  Created by John Fultz on 6/18/14.
 *  Copyright (c) 2014 Wolfram Research. All rights reserved.
 *
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "mathlink.h"
#include "WolframLibrary.h"
#include "git2.h"
#include "GitLinkRepository.h"

#include "GitLinkCommit.h"
#include "Message.h"
#include "MLExpr.h"
#include "MLHelper.h"
#include "RepoInterface.h"
#include "RemoteConnector.h"
#include "Signature.h"

#if WIN
#include <shlwapi.h>
#include <codecvt>
#endif

GitLinkRepository::GitLinkRepository(const MLExpr& expr)
	: key_()
	, repo_(NULL)
	, remoteName_(NULL)
	, committer_(NULL)
{
	MLExpr e = expr;

	// simple case...we got passed a string which is a candidate key
	if (e.isString())
		key_ = e.asString();
	// Otherwise, we have to dig inside of a GitObject or GitRepo.
	if (e.testHead("GitObject") && e.length() == 2)
		e = e.part(2);
	if (e.testHead("GitRepo") && e.length() == 1)
		e = e.part(1);
	if (e.testHead("Association") && e.length() > 0)
	{
		for (int i = 1; i <= e.length(); i++)
			if (e.part(i).isRule() && e.part(i, 1).isString() && e.part(i, 2).isString())
			{
				if (strcmp(e.part(i, 1).asString(), "GitDirectory") == 0)
					key_ = e.part(i, 2).asString();
			}
	}

	openCachedRepo_();
	if (!isValid())
		errCode_ = Message::BadRepo;
}

GitLinkRepository::GitLinkRepository(const std::string& key)
	: key_(key)
	, repo_()
	, committer_(NULL)
	, remoteName_(NULL)
{
	openCachedRepo_();
	if (!isValid())
		errCode_ = Message::BadRepo;
}

GitLinkRepository::GitLinkRepository(git_repository* repo, WolframLibraryData libData)
	: key_(PathString(git_repository_path(repo_)).native())
	, repo_(repo)
	, committer_(NULL)
	, remoteName_(NULL)
{
	ManagedRepoMap[key_] = repo_;
	if (!isValid())
		errCode_ = Message::BadRepo;
}


GitLinkRepository::~GitLinkRepository()
{
	if (revWalker_)
		git_revwalk_free(revWalker_);
	if (key_.empty() && repo_ != NULL)
		git_repository_free(repo_);
	delete committer_;
}

void GitLinkRepository::unsetKey()
{
	ManagedRepoMap.erase(key_);
	key_ = std::string();
}

const git_signature* GitLinkRepository::committer() const
{
	if (repo_ == NULL)
		return NULL;
	delete committer_;

	// recreating the signature every time assures correct commit times
	// and deals with the very rare cases where the repo's default committer changes
	committer_ = new Signature(*this);
	return *committer_;
}

void GitLinkRepository::openCachedRepo_()
{
	if (key_.empty())
		return;
	auto it = ManagedRepoMap.find(key_);
	if (it != ManagedRepoMap.end())
		repo_ = it->second;
	else if (git_repository_open(&repo_, key_.c_str()) == 0)
	{
		key_ = PathString(git_repository_path(repo_)).native();
		it = ManagedRepoMap.find(key_);
		if (it != ManagedRepoMap.end())
		{
			git_repository_free(repo_);
			repo_ = ManagedRepoMap[key_];
		}
		else
			ManagedRepoMap[key_] = repo_;
	}
	else
	{
		git_repository_free(repo_);
		repo_ = NULL;
		key_ = std::string();
	}			
}

bool GitLinkRepository::fetch(WolframLibraryData libData, const char* remoteName, const char* privateKeyFile, const MLExpr& prune, const MLExpr& downloadTags)
{
	errCode_ = errCodeParam_ = NULL;
	giterr_clear();

	if (!isValid())
	{
		errCode_ = Message::BadRepo;
		return false;
	}

	RemoteConnector connector(libData, repo_, remoteName, privateKeyFile);
	if (!connector.isValidRemote())
		errCode_ = Message::BadRemote;
	else if (!connector.fetch())
	{
		errCode_ = Message::RemoteConnectionFailed;
		errCodeParam_ = giterr_last() ? strdup(giterr_last()->message) : NULL;
	}
	if (errCode_)
		return false;
	
	git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
	opts.callbacks = connector.callbacks();

	if (prune.asBool())
		opts.prune = GIT_FETCH_PRUNE;
	else if (prune.testSymbol("Automatic"))
		opts.prune = GIT_FETCH_PRUNE_UNSPECIFIED;
	else
		opts.prune = GIT_FETCH_NO_PRUNE;

	// There is a fourth case here, GIT_REMOTE_DOWNLOAD_TAGS_AUTO, but it's pointless to support it
	// unless/until we specify fetching refspecs.
	if (downloadTags.testSymbol("None"))
		opts.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_NONE;
	else if (downloadTags.testSymbol("All"))
		opts.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_ALL;
	else
		opts.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_UNSPECIFIED;

	if (git_remote_download(connector.remote(), NULL, &opts))
	{
		errCode_ = Message::DownloadFailed;
		errCodeParam_ = strdup(giterr_last()->message);
	}

	git_remote_disconnect(connector.remote());
	if (errCode_)
		return false;

	if (git_remote_update_tips(connector.remote(), &connector.callbacks(), true, opts.download_tags, "Wolfram GitLink: fetch"))
	{
		errCode_ = Message::UpdateTipsFailed;
		errCodeParam_ = strdup(giterr_last()->message);
		return false;
	}

	return (errCode_ == NULL);
}

int GitLinkRepository::pushCallBack_(const char* ref, const char* msg, void* data)
{
	if (msg)
	{
		GitLinkRepository* repo = static_cast<GitLinkRepository*>(data);
		repo->errCode_ = Message::RefNotPushed;
		repo->errCodeParam_ = strdup(msg);
		return 1;
	}
	return 0;
}

static int packbuilder_progress(int stage, unsigned int current, unsigned int total, void* payload)
{
	char x[255];
	sprintf(x, "pack builder (%d): %d/%d", stage, current, total);

	WolframLibraryData libData = (WolframLibraryData) payload;
	MLINK lnk = libData->getMathLink(libData);
	MLPutFunction(lnk, "EvaluatePacket", 1);
	MLPutFunction(lnk, "PrintTemporary", 1);
	MLPutString(lnk, x);
	libData->processWSLINK(lnk);
	int pkt = MLNextPacket(lnk);
	if ( pkt == RETURNPKT)
		MLNewPacket(lnk);
	return 0;
}

static int transfer_progress(unsigned int current, unsigned int total, size_t bytes, void* payload)
{
	char x[255];
	sprintf(x, "transfer: %d/%d, %d bytes", current, total, (int) bytes);

	WolframLibraryData libData = (WolframLibraryData) payload;
	MLINK lnk = libData->getMathLink(libData);
	MLPutFunction(lnk, "EvaluatePacket", 1);
	MLPutFunction(lnk, "Print", 1);
	MLPutString(lnk, x);
	libData->processWSLINK(lnk);
	int pkt = MLNextPacket(lnk);
	if ( pkt == RETURNPKT)
		MLNewPacket(lnk);
	return 0;
}

bool GitLinkRepository::push(WolframLibraryData libData, const char* remoteName, const char* privateKeyFile, const char* branchName)
{
	errCode_ = errCodeParam_ = NULL;
	if (!isValid())
	{
		errCode_ = Message::BadRepo;
		return false;
	}

	RemoteConnector connector(libData, repo_, remoteName, privateKeyFile);
	if (!connector.isValidRemote())
		errCode_ = Message::BadRemote;
	else if (!connector.push())
		errCode_ = Message::RemoteConnectionFailed;

	if (errCode_)
		return false;

	git_push_options opts = GIT_PUSH_OPTIONS_INIT;
	opts.callbacks = connector.callbacks();
	git_strarray specs = {0};
	specs.count = 1;
	specs.strings = (char **) malloc(sizeof(char*));
	specs.strings[0] = (char*) branchName;

	// git_remote_push() is implemented as connect/upload/update_tips/disconnect.  Calling
	// git_remote_push would have simplified the code, but I have some special handling of
	// git_remote_connect() in the RemoteConnector class.  So breaking out the explicit steps here
	// so we don't effectively dupe the call to git_remote_connect(). Also, gets us slightly more
	// granular error-handling.
	if (git_remote_upload(connector.remote(), &specs, &opts))
		errCode_ = Message::UploadFailed;
	else if (git_remote_update_tips(connector.remote(), &connector.callbacks(), true, GIT_REMOTE_DOWNLOAD_TAGS_UNSPECIFIED,"GitLink: push"))
		errCode_ = Message::UpdateTipsFailed;
	free((void *)specs.strings);

	git_remote_disconnect(connector.remote());

	if (errCode_)
		errCodeParam_ = strdup(giterr_last()->message);

	return (errCode_ == NULL);
}

bool GitLinkRepository::setHead(const char* refName)
{
	git_reference* reference = NULL;
	errCode_ = errCodeParam_ = NULL;
	if (!isValid())
		errCode_ = Message::BadRepo;
	else if (git_reference_dwim(&reference, repo_, refName))
	{
		GitLinkCommit commit(*this, refName);
		if (commit.isValid())
			git_repository_set_head_detached(repo_, commit.oid());
		else
			errCode_ = Message::BadCommitish;
	}
	else if (git_repository_set_head(repo_, git_reference_name(reference)))
		errCode_ = Message::GitOperationFailed;

	if (reference != NULL)
		git_reference_free(reference);
	return errCode_ == NULL;
}

bool GitLinkRepository::checkoutHead(WolframLibraryData libData, const MLExpr& strategy, const MLExpr& notifyFlags)
{
	git_checkout_options opts;

	git_checkout_init_options(&opts, GIT_CHECKOUT_OPTIONS_VERSION);

	if (strategy.contains("Safe"))
		opts.checkout_strategy |= GIT_CHECKOUT_SAFE;
	if (strategy.contains("Force"))
		opts.checkout_strategy |= GIT_CHECKOUT_FORCE;
	if (strategy.contains("RecreateMissing"))
		opts.checkout_strategy |= GIT_CHECKOUT_RECREATE_MISSING;
	if (strategy.contains("AllowConflicts"))
		opts.checkout_strategy |= GIT_CHECKOUT_ALLOW_CONFLICTS;
	if (strategy.contains("RemoveUntracked"))
		opts.checkout_strategy |= GIT_CHECKOUT_REMOVE_UNTRACKED;
	if (strategy.contains("RemoveIgnored"))
		opts.checkout_strategy |= GIT_CHECKOUT_REMOVE_IGNORED;
	if (strategy.contains("UpdateOnly"))
		opts.checkout_strategy |= GIT_CHECKOUT_UPDATE_ONLY;
	if (strategy.contains("DontUpdateIndex"))
		opts.checkout_strategy |= GIT_CHECKOUT_DONT_UPDATE_INDEX; // docs say this implies GIT_CHECKOUT_DONT_WRITE_INDEX
	if (strategy.contains("NoRefresh"))
		opts.checkout_strategy |= GIT_CHECKOUT_NO_REFRESH;
	if (strategy.contains("SkipUnmerged"))
		opts.checkout_strategy |= GIT_CHECKOUT_SKIP_UNMERGED;
	if (strategy.contains("UseOurs"))
		opts.checkout_strategy |= GIT_CHECKOUT_USE_OURS;
	if (strategy.contains("UseTheirs"))
		opts.checkout_strategy |= GIT_CHECKOUT_USE_THEIRS;
	if (strategy.contains("DisablePathspecMatch"))
		opts.checkout_strategy |= GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH;
	if (strategy.contains("SkipLockedDirectories"))
		opts.checkout_strategy |= GIT_CHECKOUT_SKIP_LOCKED_DIRECTORIES;
	if (strategy.contains("DontOverwriteIgnored"))
		opts.checkout_strategy |= GIT_CHECKOUT_DONT_OVERWRITE_IGNORED;
	if (strategy.contains("ConflictStyleMerge"))
		opts.checkout_strategy |= GIT_CHECKOUT_CONFLICT_STYLE_MERGE;
	if (strategy.contains("ConflictStyleDiff3"))
		opts.checkout_strategy |= GIT_CHECKOUT_CONFLICT_STYLE_DIFF3;
	if (strategy.contains("DontRemoveExisting"))
		opts.checkout_strategy |= GIT_CHECKOUT_DONT_REMOVE_EXISTING;
	if (strategy.contains("DontWriteIndex"))
		opts.checkout_strategy |= GIT_CHECKOUT_DONT_WRITE_INDEX;
	if (strategy.contains("UpdateSubmodules"))
		opts.checkout_strategy |= GIT_CHECKOUT_UPDATE_SUBMODULES; // docs say not implemented yet
	if (strategy.contains("UpdateSubmodulesIfChanged"))
		opts.checkout_strategy |= GIT_CHECKOUT_UPDATE_SUBMODULES_IF_CHANGED; // docs say not implemented yet

	if (notifyFlags.containsKey("Conflict"))
		opts.notify_flags |= GIT_CHECKOUT_NOTIFY_CONFLICT;
	if (notifyFlags.containsKey("Dirty"))
		opts.notify_flags |= GIT_CHECKOUT_NOTIFY_DIRTY;
	if (notifyFlags.containsKey("Updated"))
		opts.notify_flags |= GIT_CHECKOUT_NOTIFY_UPDATED;
	if (notifyFlags.containsKey("Untracked"))
		opts.notify_flags |= GIT_CHECKOUT_NOTIFY_UNTRACKED;
	if (notifyFlags.containsKey("Ignored"))
		opts.notify_flags |= GIT_CHECKOUT_NOTIFY_IGNORED;
	if (notifyFlags.containsKey("All"))
		opts.notify_flags |= GIT_CHECKOUT_NOTIFY_ALL;

	if (!git_checkout_head(repo_, &opts))
		return true;

	errCode_ = Message::CheckoutFailed;
	return false;
}


void GitLinkRepository::writeProperties(MLINK lnk, bool shortForm) const
{
	if (isValid())
	{
		MLHelper helper(lnk);

		helper.beginFunction("Association");

		if (!shortForm)
		{
			git_reference* headReference = NULL;
			git_repository_head(&headReference, repo_);
			if (headReference != NULL)
			{
				const char* branchName;
				helper.putRule("HEAD", git_reference_name(headReference));
				if (!git_branch_name(&branchName, headReference))
					helper.putRule("HeadBranch", branchName);
				git_reference_free(headReference);
			}
			helper.putRule("ShallowQ", git_repository_is_shallow(repo_));
			helper.putRule("EmptyQ", git_repository_is_empty(repo_));
			helper.putRule("UnbornHeadQ", git_repository_head_unborn(repo_));
		}
		helper.putRule("BareQ", git_repository_is_bare(repo_));
		helper.putRule("GitDirectory", PathString(git_repository_path(repo_)));

		helper.putRule("WorkingDirectory");
		if (git_repository_workdir(repo_) == NULL)
			helper.putSymbol("None");
		else
			helper.putString(PathString(git_repository_workdir(repo_)));

		if (!shortForm)
		{
			helper.putRule("DetachedHeadQ", git_repository_head_detached(repo_));
			helper.putRule("Namespace", git_repository_get_namespace(repo_));
			helper.putRule("State", (git_repository_state_t) git_repository_state(repo_));

			helper.putRule("Conflicts");
			writeConflictList_(helper);

			helper.putRule("Remotes");
			writeRemotes(helper);

			helper.putRule("LocalBranches");
			writeBranchList_(helper, GIT_BRANCH_LOCAL);

			helper.putRule("RemoteBranches");
			writeBranchList_(helper, GIT_BRANCH_REMOTE);

			helper.putRule("Tags");
			writeTagList_(helper);
		}
	}
	else
		MLPutSymbol(lnk, "$Failed");
}

void GitLinkRepository::writeConflictList_(MLHelper& helper) const
{
	git_index* index;
	const git_index_entry* ancestor;
	const git_index_entry* ours;
	const git_index_entry* theirs;

	helper.beginList();
	if (!git_repository_index(&index, repo_))
	{
		git_index_conflict_iterator* it;
		git_index_conflict_iterator_new(&it, index);
		while (it && !git_index_conflict_next(&ancestor, &ours, &theirs, it))
		{
			helper.beginFunction("Association");
			helper.putRule("AncestorFileName", (ancestor != NULL) ? ancestor->path : NULL, "None");
			helper.putRule("OurFileName", (ours != NULL) ? ours->path : NULL, "None");
			helper.putRule("TheirFileName", (theirs != NULL) ? theirs->path : NULL, "None");
			helper.endFunction();
		}
		git_index_conflict_iterator_free(it);
		git_index_free(index);
	}
	helper.endList();
}

void GitLinkRepository::writeRemotes(MLHelper& helper) const
{
	git_strarray remotesList;
	helper.beginFunction("Association");
	if (!git_remote_list(&remotesList, repo_))
	{
		for (int i = 0; i < remotesList.count; i++)
		{
			git_remote* remote;
			git_strarray refspecs;
			if (git_remote_lookup(&remote, repo_, remotesList.strings[i]) != 0)
				continue;

			helper.putRule(remotesList.strings[i]);

			helper.beginFunction("Association");
			helper.putRule("FetchURL", git_remote_url(remote));
			helper.putRule("PushURL",
				(git_remote_pushurl(remote) == NULL) ?
					git_remote_url(remote) : git_remote_pushurl(remote));
			helper.putRule("FetchRefSpecs");
			helper.beginList();
			if (git_remote_get_fetch_refspecs(&refspecs, remote) == 0)
			{
				for (int j = 0; j < refspecs.count; j++)
					helper.putString(refspecs.strings[j]);
				git_strarray_free(&refspecs);
			}
			helper.endList();
			helper.putRule("PushRefSpecs");
			helper.beginList();
			if (git_remote_get_push_refspecs(&refspecs, remote) == 0)
			{
				for (int j = 0; j < refspecs.count; j++)
					helper.putString(refspecs.strings[j]);
				git_strarray_free(&refspecs);
			}
			helper.endList();
			helper.endFunction();

			git_remote_free(remote);
		}
		git_strarray_free(&remotesList);
	}
	helper.endFunction();
}

void GitLinkRepository::writeBranchList_(MLHelper& helper, git_branch_t flag) const
{
	git_branch_iterator* it;
	git_reference* ref;
	git_branch_t refType;

	helper.beginList();
	if (git_branch_iterator_new(&it, repo_, flag) == 0)
	{
		while (!git_branch_next(&ref, &refType, it))
		{
			const char* branchName;
			git_branch_name(&branchName, ref);
			helper.putString(branchName);
			git_reference_free(ref);
		}
		git_branch_iterator_free(it);
	}
	helper.endList();
}

void GitLinkRepository::writeTagList_(MLHelper& helper) const
{
	git_strarray tagNames;
	helper.beginList();
	if (!git_tag_list(&tagNames, repo_))
	{
		for (int i = 0; i < tagNames.count; i++)
			helper.putString(tagNames.strings[i]);
		git_strarray_free(&tagNames);
	}
	helper.endList();
}

git_tree* GitLinkRepository::copyTree(const MLExpr& expr)
{
	git_tree* returnValue = NULL;
	git_oid treeSha;
	bool treeShaFilled = false;

	if (expr.testSymbol("None"))
	{
		git_index* index = NULL;
		if (git_repository_index(&index, repo_) || git_index_write_tree_to(&treeSha, index, repo_))
			errCode_ = Message::NoIndex;
		else
			treeShaFilled = true;
		if (index)
			git_index_free(index);
	}
	else if (expr.isString())
	{
		if (git_oid_fromstr(&treeSha, expr.asString()))
			errCode_ = Message::BadSHA;
		else
			treeShaFilled = true;
	}

	if (treeShaFilled)
	{
		if (git_object_lookup((git_object**) &returnValue, repo_, &treeSha, GIT_OBJ_TREE))
			errCode_ = Message::NoTree;
	}
	return returnValue;
}

git_revwalk* GitLinkRepository::revWalker() const
{
	if (revWalker_ == NULL && isValid())
		git_revwalk_new(&revWalker_, repo_);
	return revWalker_;
}
