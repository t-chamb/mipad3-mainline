#!/usr/bin/env python3
"""Resolve Alpine packages and their dependencies from the APKINDEX files and unpack
them into a directory, without running apk or anything from the packages.

usage: apk-fetch.py ROOT PKG [PKG...]
Indexes are read from dl/idx/{main,community}/APKINDEX next to this script; packages
are downloaded to dl/apk/ over HTTPS and unpacked with tar as the current user.
"""
import os
import re
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = 'https://dl-cdn.alpinelinux.org/alpine/v3.24/%s/aarch64/%s-%s.apk'


def load():
    pkgs, provides = {}, {}
    for repo in ('main', 'community'):
        text = open(f'{HERE}/dl/idx/{repo}/APKINDEX', encoding='utf-8', errors='replace').read()
        for para in text.split('\n\n'):
            f = {}
            for line in para.splitlines():
                if len(line) > 2 and line[1] == ':':
                    f[line[0]] = line[2:]
            if 'P' not in f:
                continue
            f['repo'] = repo
            pkgs[f['P']] = f
            provides.setdefault(f['P'], f['P'])
            for p in f.get('p', '').split():
                provides.setdefault(re.split(r'[<>=~]', p)[0], f['P'])
    return pkgs, provides


def main():
    root, want = sys.argv[1], sys.argv[2:]
    pkgs, provides = load()
    todo, done = list(want), []
    while todo:
        name = todo.pop(0)
        if name not in pkgs:
            sys.exit(f'unknown package {name}')
        if name in done:
            continue
        done.append(name)
        for dep in pkgs[name].get('D', '').split():
            if dep.startswith('!'):
                continue
            key = re.split(r'[<>=~]', dep)[0]
            if key.startswith('/'):
                continue            # a file path such as /bin/sh: not needed for libraries
            prov = provides.get(key)
            if prov is None:
                print(f'  note: nothing provides {dep} (needed by {name})')
            elif prov not in done and prov not in todo:
                todo.append(prov)
    os.makedirs(f'{HERE}/dl/apk', exist_ok=True)
    os.makedirs(root, exist_ok=True)
    total = 0
    for name in done:
        p = pkgs[name]
        fn = f'{HERE}/dl/apk/{name}-{p["V"]}.apk'
        if not os.path.exists(fn):
            urllib.request.urlretrieve(REPO % (p['repo'], name, p['V']), fn)
        total += int(p.get('I', 0))
        subprocess.run(['tar', '-xzf', fn, '-C', root, '--warning=no-unknown-keyword',
                        '--no-same-owner', '--exclude=.PKGINFO', '--exclude=.SIGN.*',
                        '--exclude=.pre-*', '--exclude=.post-*', '--exclude=.trigger'],
                       check=True, stderr=subprocess.DEVNULL)
        print(f'{name:34s} {p["V"]:16s} {int(p.get("I", 0)) // 1024:7d} KiB')
    print(f'{len(done)} packages, {total // 1048576} MiB installed size')


if __name__ == '__main__':
    main()
