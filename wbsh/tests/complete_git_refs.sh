#!/bin/bash
# Git ref completion reads refs and config from the repository's common
# dir, so it works in a linked worktree; checkout also offers tags,
# remote-tracking branches and the branch names it can create from a
# single remote, and a reftable repository is listed through git itself.
# The repositories are hand-built: no git binary is needed.

mkdir -p _run_tmp/gitrefs
cd _run_tmp/gitrefs || exit 1
rm -rf main wt rt

mkdir -p main/.git/refs/heads/feature main/.git/refs/tags main/.git/refs/remotes/origin
mkdir -p main/.git/worktrees/wt
echo 0 > main/.git/refs/heads/master
echo 0 > main/.git/refs/heads/feature/login
echo 0 > main/.git/refs/remotes/origin/HEAD
echo 0 > main/.git/refs/remotes/origin/release
printf '# pack-refs with: peeled\n0 refs/heads/maint\n0 refs/tags/v1.0\n^1\n' > main/.git/packed-refs
printf '0 refs/remotes/upstream/release\n0 refs/remotes/upstream/topic\n' >> main/.git/packed-refs
printf '[core]\n\tbare = false\n[remote "origin"]\n\turl = a\n[remote "upstream"]\n\turl = b\n' \
	> main/.git/config
echo 'ref: refs/heads/master' > main/.git/HEAD
echo '../..' > main/.git/worktrees/wt/commondir
echo 'ref: refs/heads/feature/login' > main/.git/worktrees/wt/HEAD
mkdir wt
echo 'gitdir: ../main/.git/worktrees/wt' > wt/.git

complete_in() {
	(cd "$1" && shift && __lineedit "$@" '<TAB>')
}

echo "=== a linked worktree sees the main repository's refs and remotes"
complete_in wt 'git checkout feat'
complete_in wt 'git branch maint'
complete_in wt 'git push up'
complete_in wt 'git push origin mas'

echo "=== the main checkout gives the same answers"
complete_in main 'git checkout feat'
complete_in main 'git push up'

echo "=== checkout offers tags, remote branches and names it can create"
complete_in wt 'git checkout v'
complete_in wt 'git checkout up'
complete_in wt 'git checkout upstream/t'
complete_in wt 'git checkout top'
complete_in wt 'git checkout rel'

echo "=== switch offers branches and creatable names, not tags"
complete_in wt 'git switch top'
complete_in wt 'git switch v'

echo "=== merge, rebase and friends offer every ref"
complete_in wt 'git merge v'
complete_in wt 'git rebase origin/r'

echo "=== a reftable repository is listed by git for-each-ref"
mkdir -p rt/.git/reftable
echo 'ref: refs/heads/.invalid' > rt/.git/HEAD
git() {
	if [ "$1" = for-each-ref ]; then
		printf 'refs/heads/alpha\nrefs/heads/beta\nrefs/tags/v2.0\n'
	fi
}
complete_in rt 'git checkout al'
complete_in rt 'git checkout v'
complete_in rt 'git switch be'
