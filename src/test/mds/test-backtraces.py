
import subprocess as sub
from cStringIO import StringIO
import json
import os

from rados import (Rados)
from cephfs import (LibCephFS)

cephinst='/home/slang/dev/ceph/build-wip-bt2/src'

def flush(ceph):

    r = sub.call(
        [
            '{c}/ceph'.format(c=cephinst),
            'mds',
            'tell',
            'a',
            'injectargs',
            "'--mds_log_max_segments 2'",
        ],
        shell=True,
      )
    if (r != 0):
        raise

    ceph.shutdown()

def decode(value):

    p = sub.Popen(
        [
            '{c}/ceph-dencoder'.format(c=cephinst),
            'import',
            '-',
            'type',
            'inode_backtrace_t',
            'dump_json',
        ],
        shell=True,
        stdin=value,
        stdout=StringIO(),
      )
    if (p.returncode != 0):
        raise
    return json.loads(p.stdout)

def verify(bt, ino, values):
    if bt['ino'] != ino:
        raise
    ind = 0
    for (n, i) in values:
        if bt['ancestors'][ind]['dirino'] != i:
            raise
        if bt['ancestors'][0]['dname'] != n:
            raise
        ind++

ceph = LibCephFS()
ceph.mount()
rados = Rados()
rados.connect()

ceph.mkdir('foo')
fooi = ceph.stat('foo')['st_ino']
ceph.mkdir('foo/bar')
bari = ceph.stat('foo/bar')['st_ino']
fd = ceph.open('foo/bar/baz', os.O_CREAT | os.O_RDWR, 0644)
ceph.close(fd)

bazi = ceph.stat('foo/bar/baz')['st_ino']

flush(ceph)

ceph = LibCephFS()
ceph.mount()

ioctx = rados.open_ioctx('data')
binbt = ioctx.get_xattr('{i}.00000000'.format(bazi), 'parent')

bt = decode(binbt)

verify(bt, [('foo', fooi), ('bar', bari), ('baz', bazi)])

ioctx.close()
rados.shutdown()
ceph.shutdown()
