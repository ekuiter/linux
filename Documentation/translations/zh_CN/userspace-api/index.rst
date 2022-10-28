.. SPDX-License-Identifier: GPL-2.0
.. include:: ../disclaimer-zh_CN.rst

:Original: Documentation/userspace-api/index.rst

:翻译:

 李睿 Rui Li <me@lirui.org>

=========================
Linux 内核用户空间API指南
=========================

.. _man-pages: https://www.kernel.org/doc/man-pages/

尽管许多用户空间API的文档被记录在别处（特别是在 man-pages_ 项目中），
在代码树中仍然可以找到有关用户空间的部分信息。这个手册意在成为这些信息
聚集的地方。

.. class:: toc-title

	   目录

.. toctree::
   :maxdepth: 2

   ebpf/index

TODOList:

* no_new_privs
* seccomp_filter
* landlock
* unshare
* spec_ctrl
* accelerators/ocxl
* ioctl/index
* iommu
* media/index
* netlink/index
* sysfs-platform_profile
* vduse
* futex2

.. only::  subproject and html

   Indices
   =======

   * :ref:`genindex`
