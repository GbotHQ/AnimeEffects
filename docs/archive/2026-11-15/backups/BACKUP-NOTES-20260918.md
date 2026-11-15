# Backup — 2026-09-18 (pre-split snapshot)

Nothing in this backup is tracked by git; it lives inside `.git/` so `git status` stays clean.

## Primary artifact

    .git/backups/animeeffects-full-20260918.bundle      (44 MB, complete history, sha1)

Contains every ref in the repo at snapshot time: all local branches, all `refs/remotes/*`,
all tags, `refs/stash`, all `refs/backup/*`, plus the `worktrees/*` HEADs.

Restore anything with:

    git clone .git/backups/animeeffects-full-20260918.bundle /some/dir/restore
    # or fetch single refs into a repo:
    git fetch .git/backups/animeeffects-full-20260918.bundle 'refs/backup/*:refs/backup/restored/*'

Verified 2026-09-18: bundle verify = "records a complete history"; test clone reproduced the
tip tree hash, all 172 branch-only commits, the stash, and the bugfix worktree snapshot.

## Frozen refs created before any history work

    refs/backup/snapshot-20260918/<branch>          one per local branch (18 refs)
    refs/backup/bugfix-worktree-20260918            bca629c  dirty bugfix worktree, incl. untracked
    refs/backup/snapshot-20260918/worktree-<name>   HEAD of each of the 4 worktrees

Key tips at snapshot time:

    feat/export-ui-redesign   c9ec3c3   (172 commits over upstream/master, never pushed)
    master                    322a523   origin/master + 4 duplicate blur commits
    fix/bugfix                22181ed   + dirty worktree (38 M, 5 R, 4 RM, 2 D, 1 untracked)
    feat/docking-ui           12a793a
    feat/mipmapping           65f4fe3
    refs/stash                4fa1ec2   "On feat/folder-blend-modes: folder-opacity verification temp cases"

## The bugfix worktree snapshot (bca629c)

Created with a throwaway index (`GIT_INDEX_FILE`), so the real worktree was not touched:
50 changed paths vs `fix/bugfix` (22181ed), including the untracked `tests/support/`.
Tree: b6aeed34a2acee0ad1abef8e48ececafd69f9030

## Still recommended

1. Push a real copy off this machine:
   `git push origin refs/backup/snapshot-20260918/feat/export-ui-redesign:refs/heads/backup/export-ui-redesign-20260918`
2. The bundle lives inside `.git/`; copy it somewhere else once (it is not on a remote).
