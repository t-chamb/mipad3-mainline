#!/usr/bin/env python3
"""Print gen_init_cpio lines for the Vulkan userspace: the PowerVR ICD, the loader,
vulkaninfo/vkcube and exactly the shared libraries they need (ELF NEEDED closure),
taken from the unpacked Alpine packages in ./root."""
import os
import re
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'root')
START = ['usr/lib/libvulkan_powervr_mesa.so', 'usr/lib/libvulkan.so.1', 'usr/bin/vulkaninfo',
         'usr/bin/vkcube'] + sys.argv[1:]
# our own test programs (tests/, cross-built against this musl userspace), if built
START += [t for t in ('usr/bin/vk_tri', 'usr/bin/vk_tri_1024', 'usr/bin/vk_disp')
          if os.path.exists(os.path.join(ROOT, t))]
EXTRA = ['usr/share/vulkan/icd.d/powervr_mesa_icd.aarch64.json']


def needed(path):
    out = subprocess.run(['aarch64-linux-gnu-readelf', '-d', path],
                         capture_output=True, text=True).stdout
    return re.findall(r'\(NEEDED\)\s+Shared library: \[(.+?)\]', out)


def find(lib):
    for d in ('usr/lib', 'lib'):
        if os.path.lexists(os.path.join(ROOT, d, lib)):
            return os.path.join(d, lib)
    sys.exit(f'missing library {lib}')


paths, todo = set(), list(START) + EXTRA + [find('libc.musl-aarch64.so.1')]
while todo:
    rel = todo.pop()
    if rel in paths:
        continue
    paths.add(rel)
    full = os.path.join(ROOT, rel)
    if os.path.islink(full):
        target = os.path.normpath(os.path.join(os.path.dirname(rel), os.readlink(full)))
        todo.append(target)
    elif open(full, 'rb').read(4) == b'\x7fELF':
        todo += [find(n) for n in needed(full)]

dirs = set()
for rel in paths:
    d = os.path.dirname(rel)
    while d:
        dirs.add(d)
        d = os.path.dirname(d)
for d in sorted(dirs):
    print(f'dir /{d} 0755 0 0')
for rel in sorted(paths):
    full = os.path.join(ROOT, rel)
    if os.path.islink(full):
        print(f'slink /{rel} {os.readlink(full)} 0777 0 0')
    else:
        mode = '0755' if os.access(full, os.X_OK) else '0644'
        print(f'file /{rel} {full} {mode} 0 0')
