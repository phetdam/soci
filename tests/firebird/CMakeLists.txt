###############################################################################
#
# This file is part of CMake configuration for SOCI library
#
# Copyright (C) 2010-2013 Mateusz Loskot <mateusz@loskot.net>
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at
# http://www.boost.org/LICENSE_1_0.txt)
#
###############################################################################

soci_backend_test(
  BACKEND Firebird
  DEPENDS Firebird
  SOURCE test-firebird.cpp
  CONNSTR "service=/tmp/soci_test.fdb user=SYSDBA password=masterkey")
