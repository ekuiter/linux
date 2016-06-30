.. -*- coding: utf-8; mode: rst -*-

.. _V4L2-SDR-FMT-CU08:

*************************
V4L2_SDR_FMT_CU8 ('CU08')
*************************

*man V4L2_SDR_FMT_CU8(2)*

Complex unsigned 8-bit IQ sample


Description
===========

This format contains sequence of complex number samples. Each complex
number consist two parts, called In-phase and Quadrature (IQ). Both I
and Q are represented as a 8 bit unsigned number. I value comes first
and Q value after that.

**Byte Order..**

Each cell is one byte.



.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths:       2 1


    -  .. row 1

       -  start + 0:

       -  I'\ :sub:`0`

    -  .. row 2

       -  start + 1:

       -  Q'\ :sub:`0`




.. ------------------------------------------------------------------------------
.. This file was automatically converted from DocBook-XML with the dbxml
.. library (https://github.com/return42/sphkerneldoc). The origin XML comes
.. from the linux kernel, refer to:
..
.. * https://github.com/torvalds/linux/tree/master/Documentation/DocBook
.. ------------------------------------------------------------------------------
