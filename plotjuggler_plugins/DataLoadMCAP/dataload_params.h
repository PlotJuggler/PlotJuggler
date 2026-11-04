#pragma once

#include <QStringList>

namespace mcap
{

struct LoadParams
{
  QStringList selected_topics;
  unsigned max_array_size;
  bool clamp_large_arrays;
  bool use_timestamp = false;
  bool use_mcap_log_time;
  bool use_legacy_cdr_alignment = false;  // byte 0 alignment for RTI/legacy CDR
  int sorted_column = 0;
};

}  // namespace mcap
