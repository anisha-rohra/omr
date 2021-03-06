###############################################################################
#
# (c) Copyright IBM Corp. 2017
#
#  This program and the accompanying materials are made available
#  under the terms of the Eclipse Public License v1.0 and
#  Apache License v2.0 which accompanies this distribution.
#
#      The Eclipse Public License is available at
#      http://www.eclipse.org/legal/epl-v10.html
#
#      The Apache License v2.0 is available at
#      http://www.opensource.org/licenses/apache2.0.php
#
# Contributors:
#    Multiple authors (IBM Corp.) - initial implementation and documentation
###############################################################################


add_library(omrGtest STATIC
	omrGtest.cpp)
target_include_directories(omrGtest
	PRIVATE
		${PROJECT_SOURCE_DIR}/third_party/gtest-1.7.0/
		${PROJECT_SOURCE_DIR}/third_party/gtest-1.7.0/src
	PUBLIC
		${PROJECT_SOURCE_DIR}/third_party/gtest-1.7.0/include
)
add_library(omrGtestGlue INTERFACE)

target_include_directories(omrGtestGlue
	INTERFACE
		.
)

target_sources(omrGtestGlue INTERFACE
	${CMAKE_CURRENT_SOURCE_DIR}/argmain.cpp
)

target_link_libraries(omrGtestGlue INTERFACE omrGtest)

#TODO  system thread library should be linked in a more generic way.
if(NOT OMR_HOST_OS STREQUAL "win")
	if(NOT OMR_HOST_OS STREQUAL "zos")
		target_link_libraries(omrGtest pthread)
	endif()
endif()
#target_link_libraries(omrGtest INTERFACE omrGtestGlue)
