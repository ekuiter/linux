.. -*- coding: utf-8; mode: rst -*-

.. _VIDIOC_ENUMAUDIO:

**********************
ioctl VIDIOC_ENUMAUDIO
**********************

*man VIDIOC_ENUMAUDIO(2)*

Enumerate audio inputs


Synopsis
========

.. c:function:: int ioctl( int fd, int request, struct v4l2_audio *argp )

Arguments
=========

``fd``
    File descriptor returned by :ref:`open() <func-open>`.

``request``
    VIDIOC_ENUMAUDIO

``argp``


Description
===========

To query the attributes of an audio input applications initialize the
``index`` field and zero out the ``reserved`` array of a struct
:ref:`v4l2_audio <v4l2-audio>` and call the :ref:`VIDIOC_ENUMAUDIO`
ioctl with a pointer to this structure. Drivers fill the rest of the
structure or return an EINVAL error code when the index is out of
bounds. To enumerate all audio inputs applications shall begin at index
zero, incrementing by one until the driver returns EINVAL.

See :ref:`VIDIOC_G_AUDIO <VIDIOC_G_AUDIO>` for a description of struct
:ref:`v4l2_audio <v4l2-audio>`.


Return Value
============

On success 0 is returned, on error -1 and the ``errno`` variable is set
appropriately. The generic error codes are described at the
:ref:`Generic Error Codes <gen-errors>` chapter.

EINVAL
    The number of the audio input is out of bounds.


.. ------------------------------------------------------------------------------
.. This file was automatically converted from DocBook-XML with the dbxml
.. library (https://github.com/return42/sphkerneldoc). The origin XML comes
.. from the linux kernel, refer to:
..
.. * https://github.com/torvalds/linux/tree/master/Documentation/DocBook
.. ------------------------------------------------------------------------------
