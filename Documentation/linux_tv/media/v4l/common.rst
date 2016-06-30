.. -*- coding: utf-8; mode: rst -*-

.. _common:

###################
Common API Elements
###################
Programming a V4L2 device consists of these steps:

-  Opening the device

-  Changing device properties, selecting a video and audio input, video
   standard, picture brightness a. o.

-  Negotiating a data format

-  Negotiating an input/output method

-  The actual input/output loop

-  Closing the device

In practice most steps are optional and can be executed out of order. It
depends on the V4L2 device type, you can read about the details in
:ref:`devices`. In this chapter we will discuss the basic concepts
applicable to all devices.


.. toctree::
    :maxdepth: 1

    open
    querycap
    app-pri
    video
    audio
    tuner
    standard
    dv-timings
    controls
    format
    planar-apis
    crop
    selection-api
    streaming-par




.. ------------------------------------------------------------------------------
.. This file was automatically converted from DocBook-XML with the dbxml
.. library (https://github.com/return42/sphkerneldoc). The origin XML comes
.. from the linux kernel, refer to:
..
.. * https://github.com/torvalds/linux/tree/master/Documentation/DocBook
.. ------------------------------------------------------------------------------
