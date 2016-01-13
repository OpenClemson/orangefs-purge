#!/usr/bin/env python
from __future__ import print_function
import os
import random
import sys
import time

MIN_DIR_WIDTH = 0
MAX_DIR_WIDTH = 3

MIN_FILE_WIDTH = 0
MAX_FILE_WIDTH = 10

NOW = int(time.time())
FILE_NOT_USED_MAX_SECS = 60 * 60 * 24 * 60 # 60 days in seconds 

def error(*objs):
    print("ERROR: ", *objs, file=sys.stderr)

def mkfile(fpath, times=None): # size=None
    with open(fpath, 'a'):
        os.utime(fpath, times)

def mktree(parent, dirdepth, dirwidth, fwidth):
    if dirdepth == 0:
        return

    for i in range(0, fwidth):
        last_used = NOW - random.randint(0, FILE_NOT_USED_MAX_SECS)
        times = (last_used, last_used)
        mkfile(parent + os.sep + 'f' + str(i), times)

    for i in range(0, dirwidth):
        dpath = parent + os.sep + 'd' + str(i)
        os.mkdir(dpath) # NOTE: no error checking here
        mktree(dpath,
               dirdepth - 1,
               random.randint(MIN_DIR_WIDTH, MAX_DIR_WIDTH),
               random.randint(MIN_FILE_WIDTH, MAX_FILE_WIDTH))

def mkuser_dirs(parent, num_users):
    user_dirs = []
    letters = 'abcdefghijklmnopqrstuvwxyz'
    for i in range(0, num_users):
        name = ''
        letter = i % len(letters)
        letter_count = (i / len(letters)) + 1
        for c in range(0, letter_count):
            name += letters[letter]
        user_dir = parent + os.sep + name
        os.mkdir(user_dir)
        user_dirs.append(user_dir)
    return user_dirs

if __name__ == '__main__':

    if(len(sys.argv[1]) == 0):
        error("Please provide an absolute path as the first argument to this script!")
        exit(1)

    num_users = 52

    # Ensure that supplied directory path is writable and executable
    if not os.access(sys.argv[1], os.W_OK | os.X_OK):
        error("Please verify that the supplied absolute path exists and is writable!")
        exit(1)

    user_dirs = mkuser_dirs(sys.argv[1], num_users)

    #print(user_dirs)
    for user_dir in user_dirs:
        print("generating random directory tree under path: " + user_dir)
        mktree(user_dir,
               5,
               random.randint(1, MAX_DIR_WIDTH),
               random.randint(MIN_FILE_WIDTH, MAX_FILE_WIDTH))
    exit(0)
