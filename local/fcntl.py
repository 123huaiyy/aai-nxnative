# Windows stand-in for the POSIX fcntl module, for nxnative's tools run on Windows (one build at a time, so advisory locks can be no-ops).
LOCK_SH, LOCK_EX, LOCK_NB, LOCK_UN = 1, 2, 4, 8
F_GETFL, F_SETFL, F_GETFD, F_SETFD, FD_CLOEXEC = 3, 4, 1, 2, 1


def flock(fd, operation):
    return None


def lockf(fd, cmd, len=0, start=0, whence=0):
    return None


def fcntl(fd, cmd, arg=0):
    return 0
