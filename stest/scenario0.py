import os
import itertools
from walb_cmd import *


s0 = Server('s0', '10000', K_STORAGE, None)
s1 = Server('s1', '10001', K_STORAGE, None)
s2 = Server('s2', '10002', K_STORAGE, None)
p0 = Server('p0', '10100', K_PROXY, None)
p1 = Server('p1', '10101', K_PROXY, None)
p2 = Server('p2', '10102', K_PROXY, None)
a0 = Server('a0', '10200', K_ARCHIVE, 'vg0')
a1 = Server('a1', '10201', K_ARCHIVE, 'vg1')
#a2 = Server('a2', '10202', None)

WORK_DIR = os.getcwd() + '/stest/tmp/'
isDebug = True

config = Config(isDebug, os.getcwd() + '/binsrc/',
                WORK_DIR, [s0, s1], [p0, p1], [a0, a1])


WDEV_PATH = '/dev/walb/0'
VOL = 'vol0'

set_config(config)


def setup_test():
    run_command(['/bin/rm', '-rf', WORK_DIR])
    if os.path.isdir('/dev/vg0'):
        for f in os.listdir('/dev/vg0'):
            if f[0] == 'i':
                run_command(['/sbin/lvremove', '-f', '/dev/vg0/' + f])
    make_dir(WORK_DIR)
    kill_all_servers()
    startup_all()


def test_n1():
    """
        full-backup -> sha1 -> restore -> sha1
    """
    print "test_n1:full-backup"
    init(s0, VOL, WDEV_PATH)
    write_random(WDEV_PATH, 1)
    md0 = get_sha1(WDEV_PATH)
    gid = full_backup(s0, VOL)
    restore_and_verify_sha1('test_n1', md0, a0, VOL, gid)


def test_n2():
    """
        write -> sha1 -> snapshot -> restore -> sha1
    """
    print "test_n2:snapshot"
    write_random(WDEV_PATH, 1)
    md0 = get_sha1(WDEV_PATH)
    gid = snapshot_sync(s0, VOL, [a0])
    print "gid=", gid
    print list_restorable(a0, VOL)
    restore_and_verify_sha1('test_n2', md0, a0, VOL, gid)


def test_n3():
    """
        hash-backup -> sha1 -> restore -> sha1
    """
    print "test_n3:hash-backup"
    set_slave_storage(s0, VOL)
    write_random(WDEV_PATH, 1)
    md0 = get_sha1(WDEV_PATH)
    gid = hash_backup(s0, VOL)
    print "gid=", gid
    restore_and_verify_sha1('test_n3', md0, a0, VOL, gid)


def printL(aL, bL):
    print '[',
    for a in aL:
        print a.name,
    print '], [',
    for b in bL:
        print b.name,
    print ']'


def test_stop(stopL, startL):
    printL(stopL, startL)
    t = startWriting(WDEV_PATH)

    for s in stopL:
        stop(s, VOL)
        time.sleep(0.1)

    time.sleep(0.5)

    for s in startL:
        start(s, VOL)
        time.sleep(0.1)

    stopWriting(t)

    md0 = get_sha1(WDEV_PATH)
    gid = snapshot_sync(s0, VOL, [a0])
    restore_and_verify_sha1('test_stop', md0, a0, VOL, gid)


def test_n4():
    """
        stop -> start -> snapshot -> sha1
    """
    print "test_n4:stop"
    setLL = [[s0], [p0], [a0], [s0, p0], [s0, a0], [p0, a0], [s0, p0, a0]]
    for setL in setLL:
        for stopL in itertools.permutations(setL):
            for startL in itertools.permutations(setL):
                test_stop(stopL, startL)
#                printL(stopL, startL)


def test_n5():
    """
        apply -> sha1
    """
    print "test_n5:apply"
    t = startWriting(WDEV_PATH)
    time.sleep(0.5)
    gid = snapshot_sync(s0, VOL, [a0])
    time.sleep(0.5)
    stopWriting(t)
    md0 = get_sha1_of_restorable(a0, VOL, gid)
    apply_diff(a0, VOL, gid)
    restore_and_verify_sha1('test_n5', md0, a0, VOL, gid)


def test_n6():
    """
        merge -> sha1
    """
    print "test_n6:merge"
    t = startWriting(WDEV_PATH)
    time.sleep(0.5)
    gidB = snapshot_sync(s0, VOL, [a0])
    time.sleep(1)
    # create more than two diff files
    stop(s0, VOL)
    stop(p0, VOL, 'empty')
    start(s0, VOL)
    start(p0, VOL)
    time.sleep(1)
    gidE = snapshot_sync(s0, VOL, [a0])
    gidL = list_restorable(a0, VOL, 'all')
    posB = gidL.index(gidB)
    posE = gidL.index(gidE)
    print "gidB", gidB, "gidE", gidE, "gidL", gidL
    if posE - posB < 2:
        stopWriting(t)
        raise Exception('test_n6:bad range', gidB, gidE, gidL)
    time.sleep(0.5)
    stopWriting(t)
    # merge gidB and gidE

    md0 = get_sha1_of_restorable(a0, VOL, gidE)
    merge_diff(a0, VOL, gidB, gidE)
    print "merged gidL", list_restorable(a0, VOL, 'all')
    restore_and_verify_sha1('test_n6', md0, a0, VOL, gidE)


def test_n7():
    """
        replicate (no synchronizing, full) -> sha1
    """
    replicate(a0, VOL, a1, False)
    list0 = list_restorable(a0, VOL)
    list1 = list_restorable(a1, VOL)
    print 'list0', list0
    print 'list1', list1
    if list0 != list1:
        raise Exception('test_n7:list differ', list0, list1)
    gid = list0[-1]
    md0 = get_sha1_of_restorable(a0, VOL, gid)
    md1 = get_sha1_of_restorable(a0, VOL, gid)
    print 'md0', md0
    print 'md1', md1
    if md0 != md1:
        raise Exception('test_n7:md differ', md0, md1)


def main():
    setup_test()
    test_n1()
#    test_n2()
#    test_n3()
#    test_n4()
#    test_n5()
#    test_n6()
    test_n7()


if __name__ == "__main__":
    main()
