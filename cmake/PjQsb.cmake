# SPDX-License-Identifier: MPL-2.0
#
# pj_find_host_qsb(<out-var> <who>): path of the host qsb that re-bakes committed
# .qsb shader packs for the configure-time hash checks. Fails the configure,
# naming <who>, when Qt ShaderTools' qsb is missing.
function(pj_find_host_qsb out_var who)
  if(NOT TARGET Qt6::qsb)
    message(FATAL_ERROR "${who} shader verification requires the host Qt6::qsb tool")
  endif()
  get_target_property(_qsb Qt6::qsb IMPORTED_LOCATION)
  if(NOT _qsb OR _qsb MATCHES "-NOTFOUND$")
    get_target_property(_qsb Qt6::qsb IMPORTED_LOCATION_RELWITHDEBINFO)
  endif()
  if(NOT EXISTS "${_qsb}")
    message(FATAL_ERROR "${who} cannot locate the host qsb executable")
  endif()
  set(${out_var} "${_qsb}" PARENT_SCOPE)
endfunction()
