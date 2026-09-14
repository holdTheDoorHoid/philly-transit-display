// Single Unity entry point for the test_core suite. Each area's tests live in their own
// test_*.cpp file (for readability); this file just declares and runs all of them, and is the
// only one that defines setUp/tearDown/main (Unity requires exactly one of each per binary).
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

// test_gtfsrt_stream.cpp
void test_gtfsrt_known_values_and_header_timestamp();
void test_gtfsrt_identical_across_chunk_sizes();
void test_gtfsrt_route_and_stop_filters();
void test_gtfsrt_oversized_entity_skipped_without_corrupting_stream();
void test_gtfsrt_oversized_entity_respects_custom_cap();
void test_gtfsrt_empty_stream_has_zero_counts();

// test_timeparse.cpp
void test_bus_schedule_time_edt();
void test_bus_schedule_time_est();
void test_bus_schedule_time_midnight_and_noon_edge();
void test_bus_schedule_time_next_day_rollover();
void test_bus_schedule_time_invalid_input();
void test_arrivals_time_edt();
void test_arrivals_time_invalid_input();
void test_dst_spring_forward_boundary();
void test_dst_fall_back_boundary();
void test_dst_mid_summer_is_edt();

// test_septa.cpp
void test_parse_transitview_fixture();
void test_parse_transitview_bare_empty_array_is_not_an_error();
void test_parse_bus_schedules_fixture();
void test_parse_bus_schedules_error_400_shape();
void test_parse_bus_schedules_501_body_is_actually_valid_shaped();
void test_parse_bus_schedules_subway_route_id();
void test_parse_alerts_fixture();
void test_parse_alerts_empty_array();
void test_parse_rail_arrivals_fixture();
void test_parse_rail_arrivals_error_shape();
void test_rail_line_lookup();

// test_merge.cpp
void test_merge_stop_joins_rt_and_tv_by_trip_id();
void test_merge_stop_drops_stale_arrivals();
void test_merge_stop_subway_schedule_only();
void test_merge_rail_direction_filter_and_status_mapping();
void test_merge_rail_on_time_status();
void test_merge_rail_line_filter();
void test_merge_rail_unrecognized_line_code_matches_nothing();
void test_merge_rail_no_direction_filter_returns_both();

// test_septa_source.cpp
void test_septa_url_builders();
void test_alert_route_id_for_bus_and_trolley();
void test_alert_route_id_for_subway_uses_rr_prefix();
void test_alert_route_id_for_rail_uses_lookup_table();
void test_alert_route_id_for_empty_route();
void test_poll_bus_stops_merges_both_configured_stops();
void test_poll_bus_stops_subway_is_schedule_only();
void test_poll_bus_stops_transport_failure_marks_snapshot();
void test_poll_rail_stops_merges_by_direction();

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  RUN_TEST(test_gtfsrt_known_values_and_header_timestamp);
  RUN_TEST(test_gtfsrt_identical_across_chunk_sizes);
  RUN_TEST(test_gtfsrt_route_and_stop_filters);
  RUN_TEST(test_gtfsrt_oversized_entity_skipped_without_corrupting_stream);
  RUN_TEST(test_gtfsrt_oversized_entity_respects_custom_cap);
  RUN_TEST(test_gtfsrt_empty_stream_has_zero_counts);

  RUN_TEST(test_bus_schedule_time_edt);
  RUN_TEST(test_bus_schedule_time_est);
  RUN_TEST(test_bus_schedule_time_midnight_and_noon_edge);
  RUN_TEST(test_bus_schedule_time_next_day_rollover);
  RUN_TEST(test_bus_schedule_time_invalid_input);
  RUN_TEST(test_arrivals_time_edt);
  RUN_TEST(test_arrivals_time_invalid_input);
  RUN_TEST(test_dst_spring_forward_boundary);
  RUN_TEST(test_dst_fall_back_boundary);
  RUN_TEST(test_dst_mid_summer_is_edt);

  RUN_TEST(test_parse_transitview_fixture);
  RUN_TEST(test_parse_transitview_bare_empty_array_is_not_an_error);
  RUN_TEST(test_parse_bus_schedules_fixture);
  RUN_TEST(test_parse_bus_schedules_error_400_shape);
  RUN_TEST(test_parse_bus_schedules_501_body_is_actually_valid_shaped);
  RUN_TEST(test_parse_bus_schedules_subway_route_id);
  RUN_TEST(test_parse_alerts_fixture);
  RUN_TEST(test_parse_alerts_empty_array);
  RUN_TEST(test_parse_rail_arrivals_fixture);
  RUN_TEST(test_parse_rail_arrivals_error_shape);
  RUN_TEST(test_rail_line_lookup);

  RUN_TEST(test_merge_stop_joins_rt_and_tv_by_trip_id);
  RUN_TEST(test_merge_stop_drops_stale_arrivals);
  RUN_TEST(test_merge_stop_subway_schedule_only);
  RUN_TEST(test_merge_rail_direction_filter_and_status_mapping);
  RUN_TEST(test_merge_rail_on_time_status);
  RUN_TEST(test_merge_rail_line_filter);
  RUN_TEST(test_merge_rail_unrecognized_line_code_matches_nothing);
  RUN_TEST(test_merge_rail_no_direction_filter_returns_both);

  RUN_TEST(test_septa_url_builders);
  RUN_TEST(test_alert_route_id_for_bus_and_trolley);
  RUN_TEST(test_alert_route_id_for_subway_uses_rr_prefix);
  RUN_TEST(test_alert_route_id_for_rail_uses_lookup_table);
  RUN_TEST(test_alert_route_id_for_empty_route);
  RUN_TEST(test_poll_bus_stops_merges_both_configured_stops);
  RUN_TEST(test_poll_bus_stops_subway_is_schedule_only);
  RUN_TEST(test_poll_bus_stops_transport_failure_marks_snapshot);
  RUN_TEST(test_poll_rail_stops_merges_by_direction);

  return UNITY_END();
}
