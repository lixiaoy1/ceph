================================
 RBD Persistent Write-back Cache
================================

.. index:: Ceph Block Device; Persistent Write-back Cache

Persistent Write-back Cache
===========================

Persistent Write back cache provides a persistent, fault-tolerant write-back
cache for Ceph RBD.
This cache is LBA-based, ordered write-back cache which maintains checkpoints
internally so that writes that get flushed back  to the cluster are always
crash consistent. Even if the client cache is lost entirely, the disk image is
holding a valid file system that looks like it is just a little bit stale.
Currently this cache uses PMEM devices as cache devices, and later SSDs will
be supported.

Usage
=====

Persistent Write-back Image Cache manages the cache data in a persistent device. It
looks for and creates cache files in a configured directory.
The cache appends each update request (write/discard etc.) to a log, and used
repeatedly.
It provides two persistence modes. In persistent-on-write mode, the writes
are completed only when they are persisted to the cache device and will be
readable after a crash. In persistent-on-flush mode, the writes are completed
as soon as it no longer needs the callers' data buffer to complete the writes,
but does not guarantee that writes will be readable after a crash. These data
are persisted to the cache device when a flush request is received.
Initially it is in persistent-on-write mode, and it switches to
persistent-on-flush mode if a flush request is received.
Persistent Write-back Image Cache can't be enabled without exclusive lock.
It tries to enable the write-back cache only when the exclusive lock is got.

Enable Persistent Write back Image Cache
========================================

To enable RBD persistent write-back image cache, the following Ceph settings
need to enable in the ``[client]`` `section`_ of your ``ceph.conf`` file::

        rbd rwl enabled = true
        rbd plugins = pwl_cache


RBD commands
============

Show Status
-----------



Discard Cache
-------------

To discard a cache file with ``rbd``, specify the ``image-cache invalidate``
option, the pool name and the image name.  ::

        rbd image-cache invalidate {pool-name}/{image-name}

For example::

        rbd image-cache invalidate rbd/foo

