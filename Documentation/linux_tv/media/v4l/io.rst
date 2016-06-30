.. -*- coding: utf-8; mode: rst -*-

.. _io:

############
Input/Output
############
The V4L2 API defines several different methods to read from or write to
a device. All drivers exchanging data with applications must support at
least one of them.

The classic I/O method using the :c:func:`read()` and
:c:func:`write()` function is automatically selected after opening a
V4L2 device. When the driver does not support this method attempts to
read or write will fail at any time.

Other methods must be negotiated. To select the streaming I/O method
with memory mapped or user buffers applications call the
:ref:`VIDIOC_REQBUFS <vidioc-reqbufs>` ioctl. The asynchronous I/O
method is not defined yet.

Video overlay can be considered another I/O method, although the
application does not directly receive the image data. It is selected by
initiating video overlay with the :ref:`VIDIOC_S_FMT <vidioc-g-fmt>`
ioctl. For more information see :ref:`overlay`.

Generally exactly one I/O method, including overlay, is associated with
each file descriptor. The only exceptions are applications not
exchanging data with a driver ("panel applications", see :ref:`open`)
and drivers permitting simultaneous video capturing and overlay using
the same file descriptor, for compatibility with V4L and earlier
versions of V4L2.

``VIDIOC_S_FMT`` and ``VIDIOC_REQBUFS`` would permit this to some
degree, but for simplicity drivers need not support switching the I/O
method (after first switching away from read/write) other than by
closing and reopening the device.

The following sections describe the various I/O methods in more detail.


.. toctree::
    :maxdepth: 1

    rw
    mmap
    userp
    dmabuf
    async
    buffer
    field-order




.. ------------------------------------------------------------------------------
.. This file was automatically converted from DocBook-XML with the dbxml
.. library (https://github.com/return42/sphkerneldoc). The origin XML comes
.. from the linux kernel, refer to:
..
.. * https://github.com/torvalds/linux/tree/master/Documentation/DocBook
.. ------------------------------------------------------------------------------
