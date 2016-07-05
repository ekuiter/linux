.. -*- coding: utf-8; mode: rst -*-

.. _DMX_SET_SOURCE:

DMX_SET_SOURCE
==============

Description
-----------

This ioctl is undocumented. Documentation is welcome.

Synopsis
--------

.. c:function:: int ioctl(fd, int request = DMX_SET_SOURCE, dmx_source_t *)

Arguments
----------



.. flat-table::
    :header-rows:  0
    :stub-columns: 0


    -  .. row 1

       -  int fd

       -  File descriptor returned by a previous call to open().

    -  .. row 2

       -  int request

       -  Equals DMX_SET_SOURCE for this command.

    -  .. row 3

       -  dmx_source_t *

       -  Undocumented.


Return Value
------------

On success 0 is returned, on error -1 and the ``errno`` variable is set
appropriately. The generic error codes are described at the
:ref:`Generic Error Codes <gen-errors>` chapter.


