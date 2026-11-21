#!/usr/bin/env python3
"""Export RetroVega's history from this repository into a repository of its own.

    scripts/retrovega-export.py [DEST] [--ref REF] [--after COMMIT] [--branch NAME]

DEST (default ~/dev/retrovega) gets the retrovega/ directory at its top level, with every commit of this repository
that changed it: each one rewritten so that its tree is retrovega/'s contents at that commit, its author, committer,
dates and message kept, and a last line "RetroVega-Source: dosbox3d <hash>" naming the commit it came from.  Commits
that changed nothing in retrovega/ are left out; merges stay merges (their sides folded onto the nearest exported
commit).  DEST is made and `git init`-ed when it is not a repository yet.

This repository stays the source of truth: the export is a copy, made again whenever wanted.

Again later (new changes to RetroVega): the export continues on top of DEST's branch.  By default it reads the
RetroVega-Source line of DEST's newest commit and exports what came after that commit; --after COMMIT names the source
commit to start after instead (it and everything before it are taken as already in DEST).  Without either, on an
empty DEST, the whole history is exported.

The rewriting is deterministic: the same source commits on the same base give the same hashes.  Nothing in this
repository changes but objects added to its store and a temporary ref (refs/retrovega-export/tip), deleted at the end.
"""
import argparse
import os
import subprocess
import sys

SUBDIR = 'retrovega'
TRAILER = 'RetroVega-Source: dosbox3d '


def git(*args, cwd=None, input=None, env=None):
    r = subprocess.run(['git'] + list(args), cwd=cwd, input=input, env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit('git %s failed:\n%s' % (' '.join(args), r.stderr.strip()))
    return r.stdout


def git_ok(*args, cwd=None):
    return subprocess.run(['git'] + list(args), cwd=cwd, capture_output=True, text=True).returncode == 0


def subtrees(src, commits):
    """commit -> the tree of retrovega/ in it (None where it has none), in one batch"""
    out = git('cat-file', '--batch-check=%(objectname) %(objecttype)', cwd=src,
              input=''.join('%s:%s\n' % (c, SUBDIR) for c in commits))
    trees = {}
    for c, line in zip(commits, out.splitlines()):
        parts = line.split()
        trees[c] = parts[0] if len(parts) == 2 and parts[1] == 'tree' else None
    return trees


def commit_info(src, c):
    """author/committer names, emails, dates and the message, as commit-tree wants them"""
    raw = git('cat-file', 'commit', c, cwd=src)
    head, _, msg = raw.partition('\n\n')
    env = dict(os.environ)
    for line in head.splitlines():
        for key, who in (('author ', 'AUTHOR'), ('committer ', 'COMMITTER')):
            if line.startswith(key):
                rest = line[len(key):]
                name, _, tail = rest.partition(' <')
                email, _, date = tail.partition('> ')
                env['GIT_%s_NAME' % who] = name
                env['GIT_%s_EMAIL' % who] = email
                env['GIT_%s_DATE' % who] = date
    return env, msg


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('dest', nargs='?', default=os.path.expanduser('~/dev/retrovega'))
    ap.add_argument('--ref', default='HEAD', help='the source commit to export up to (default HEAD)')
    ap.add_argument('--after', help='the source commit already exported: export only what came after it')
    ap.add_argument('--branch', default='master', help="DEST's branch (default master)")
    a = ap.parse_args()

    src = git('rev-parse', '--show-toplevel', cwd=os.path.dirname(os.path.abspath(__file__))).strip()   # (this script's own repository, wherever it is run from)
    tip = git('rev-parse', '--verify', a.ref + '^{commit}', cwd=src).strip()
    dest = os.path.abspath(os.path.expanduser(a.dest))
    os.makedirs(dest, exist_ok=True)
    if not git_ok('rev-parse', '--git-dir', cwd=dest) or \
            os.path.realpath(git('rev-parse', '--show-toplevel', cwd=dest).strip()) != os.path.realpath(dest):
        git('init', '-q', '-b', a.branch, cwd=dest)
        print('%s: a new repository' % dest)
    base = git('rev-parse', '--verify', '-q', 'refs/heads/' + a.branch, cwd=dest).strip() \
        if git_ok('rev-parse', '--verify', '-q', 'refs/heads/' + a.branch, cwd=dest) else None

    on_branch = git_ok('symbolic-ref', '-q', 'HEAD', cwd=dest) and         git('symbolic-ref', '-q', 'HEAD', cwd=dest).strip() == 'refs/heads/' + a.branch
    if on_branch and base and git('status', '--porcelain', '--untracked-files=no', cwd=dest).strip():
        sys.exit('%s has uncommitted changes on %s: commit or drop them first (the export moves the branch)'
                 % (dest, a.branch))

    after = None
    if a.after:
        after = git('rev-parse', '--verify', a.after + '^{commit}', cwd=src).strip()
    elif base:
        for line in reversed(git('log', '-1', '--format=%B', base, cwd=dest).strip().splitlines()):
            if line.startswith(TRAILER):
                after = line[len(TRAILER):].strip()
                break
        if not after:
            sys.exit('%s has commits on %s but none says where it came from (%s...): name it with --after'
                     % (dest, a.branch, TRAILER))
        if not git_ok('cat-file', '-e', after + '^{commit}', cwd=src):
            sys.exit('%s came from %s, which this repository does not have' % (dest, after))
    if after and base is None:
        sys.exit('--after needs commits in %s to continue on; it has none on %s' % (dest, a.branch))
    if after and not git_ok('merge-base', '--is-ancestor', after, tip, cwd=src):
        sys.exit('%s is not an ancestor of %s' % (after, a.ref))

    # every commit up to the tip (after `after`), parents first, with its parents
    rng = [tip] + (['^' + after] if after else [])
    order, parents = [], {}
    for line in git('rev-list', '--topo-order', '--reverse', '--parents', *rng, cwd=src).splitlines():
        c, *ps = line.split()
        order.append(c)
        parents[c] = ps
    if not order:
        print('nothing new after %s' % after[:10])
        return
    trees = subtrees(src, order + ([after] if after else []))
    if base:   # (DEST's objects into this store, so the new commits can name it as their parent)
        git('fetch', '-q', '--no-tags', dest, '+refs/heads/%s:refs/retrovega-export/base' % a.branch, cwd=src)
    # a source commit -> the exported commit standing for it (None: no RetroVega yet); outside the range: DEST's branch
    mapped = {}
    tree_of = {}               # exported commit -> its tree
    if base:
        tree_of[base] = git('rev-parse', base + '^{tree}', cwd=src).strip()

    def mp(p):
        return mapped[p] if p in mapped else base

    made = 0
    for c in order:
        ps = []
        for p in parents[c]:
            m = mp(p)
            if m and m not in ps:
                ps.append(m)
        if len(ps) > 1:        # (a side already in another: not a parent of its own)
            ps = [p for p in ps if not any(q != p and git_ok('merge-base', '--is-ancestor', p, q, cwd=src) for q in ps)]
        t = trees[c]
        if t is None:          # (no RetroVega in it: before it began)
            mapped[c] = ps[0] if ps else None
            continue
        if len(ps) == 1 and tree_of[ps[0]] == t:   # (nothing changed in it)
            mapped[c] = ps[0]
            continue
        env, msg = commit_info(src, c)
        msg = msg.rstrip('\n') + '\n\n' + TRAILER + c + '\n'
        args = ['commit-tree', t]
        for p in ps:
            args += ['-p', p]
        new = git(*args, cwd=src, input=msg, env=env).strip()
        mapped[c] = new
        tree_of[new] = t
        made += 1
    new_tip = mp(tip)
    if not new_tip or new_tip == base:
        print('nothing new in %s/ after %s' % (SUBDIR, (after or 'the start')[:10]))
        git('update-ref', '-d', 'refs/retrovega-export/base', cwd=src) if base else None
        return
    git('update-ref', 'refs/retrovega-export/tip', new_tip, cwd=src)
    git('fetch', '-q', '--no-tags', '--update-head-ok', src, 'refs/retrovega-export/tip:refs/heads/%s' % a.branch, cwd=dest)
    git('update-ref', '-d', 'refs/retrovega-export/tip', cwd=src)
    if base:
        git('update-ref', '-d', 'refs/retrovega-export/base', cwd=src)
    # the work tree, when the branch is the one checked out there: brought up to the new tip
    if on_branch:
        if base is None:
            git('checkout', '-q', '-f', a.branch, cwd=dest)
        else:
            git('read-tree', '-u', '-m', base, new_tip, cwd=dest)
    print('%s: %d commits exported onto %s (%s), up to %s of this repository'
          % (dest, made, a.branch, new_tip[:10], tip[:10]))


if __name__ == '__main__':
    main()
