#!/bin/bash
# Update the various branches and tags

add_remote() {
    if ! git remote get-url $1 >/dev/null 2>&1; then
        git remote add $1 $2
    fi
}

add_remote linus        https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
add_remote stable       https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
add_remote rt           https://git.kernel.org/pub/scm/linux/kernel/git/rt/linux-rt-devel.git
add_remote rt-stable    https://git.kernel.org/pub/scm/linux/kernel/git/rt/linux-stable-rt.git
add_remote remoteproc   https://git.kernel.org/pub/scm/linux/kernel/git/remoteproc/linux.git
add_remote wam          git@github.com:wmamills/linux.git

git fetch --all

# get rid of all next-* tags
for i in $(git tag | grep "^next-"); do git tag -d $i; done

# get rid of all uX.Y tags
for i in $(git tag | grep "^u"); do git tag -d $i; done

# get rid of stray "latest" tag from upstream
git tag -d latest

# get rid of all -rc tags but keep the last 3 releases worth
for i in $(git tag | grep "\-rc" | grep -v "v5.1[876]"); do git tag -d $i; done

# optional, get rid of all word prefixed tags (would include next-*)
for i in $(git tag | grep -v "^v"); do git tag -d $i; done

git push wam linus/master:refs/heads/linus/master
git push wam remoteproc/for-next:refs/heads/remoteproc/for-next
