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
void test_gtfsrt_retention_cap_holds_at_the_cap_with_500_matching_entities();
void test_gtfsrt_retention_global_cap_across_many_stops();
void test_gtfsrt_long_identifiers_are_truncated();
void test_gtfsrt_complete_body_reports_complete();
void test_gtfsrt_truncated_body_reports_truncated();
void test_gtfsrt_empty_body_is_complete_not_truncated();
void test_gtfsrt_malformed_entity_is_counted_not_fatal();
void test_gtfsrt_trip_level_canceled_is_surfaced();
void test_gtfsrt_fixture_updates_carry_the_header_timestamp();
void test_gtfsrt_reset_decodes_a_second_feed_identically();
void test_gtfsrt_reset_clears_a_half_parsed_feed();
void test_gtfsrt_borrowed_retention_block_is_never_reallocated();
void test_gtfsrt_retention_cap_can_be_sized_below_the_default();
void test_gtfsrt_reset_grows_the_entity_cap_when_asked();

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
void test_parse_int_bounded_rejects_overlong_digit_runs();
void test_parse_int_strict_rejects_trailing_garbage();
void test_time_parsers_reject_overlong_fields();

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
void test_parse_bus_schedules_caps_entries_and_keeps_the_nearest();
void test_parse_transitview_caps_vehicles();
void test_parse_transitview_filter_keeps_only_the_wanted_trips();
void test_parse_transitview_default_filter_keeps_every_vehicle();
void test_parse_transitview_a_filtered_out_vehicle_is_not_a_drop();
void test_parse_transitview_the_cap_counts_only_vehicles_the_filter_wanted();
void test_transitview_cap_is_sixteen_slots();
void test_transitview_trip_filter_changes_nothing_the_merge_reads();
void test_parse_transitview_truncates_long_identifiers();
void test_parse_transitview_absurd_numbers_do_not_overflow();
void test_rail_line_lookup_by_code_or_display_name();
void test_first_upcoming_schedule_time_matches_the_parser();
void test_first_upcoming_schedule_time_skips_entries_already_past();
void test_first_upcoming_schedule_time_reads_the_wrong_service_day_fixture();
void test_first_upcoming_schedule_time_tolerates_junk_and_truncation();

// test_merge.cpp
void test_merge_stop_joins_rt_and_tv_by_trip_id();
void test_merge_stop_drops_stale_arrivals();
void test_merge_stop_subway_schedule_only();
void test_merge_rail_direction_filter_and_status_mapping();
void test_merge_rail_on_time_status();
void test_merge_rail_line_filter();
void test_merge_rail_unrecognized_line_code_matches_nothing();
void test_merge_rail_no_direction_filter_returns_both();
void test_merge_stop_dedupes_scheduled_rows_by_trip_id();
void test_merge_stop_ignores_other_route_schedule_entries();
void test_merge_stop_live_arrival_does_not_match_other_route_schedule();
void test_sched_route_matches_is_case_insensitive_for_bus();
void test_merge_stop_subway_route_alias_from_fixture();
void test_sched_route_matches_unknown_subway_accepts_any();
void test_merge_stop_keeps_matched_static_trip_id();
void test_merge_stop_skipped_without_time_is_shown_and_consumes_schedule();
void test_merge_stop_skipped_with_time_keeps_prediction();
void test_merge_stop_canceled_trip_suppresses_its_schedule_row();
void test_merge_stop_no_data_leaves_the_schedule_row();
void test_merge_stop_plain_schedule_row_is_distinct();
void test_merge_stop_fresh_feed_is_live();
void test_merge_stop_old_feed_is_stale_with_no_live_rows();
void test_merge_stop_future_dated_feed_is_stale();
void test_merge_stop_missing_feed_timestamp_keeps_live_rows();
void test_merge_stop_subway_schedule_failure_is_unavailable();
void test_merge_stop_live_failure_falls_back_to_schedule_only();
void test_merge_rail_accepts_line_display_name_in_config();
void test_merge_rail_failed_fetch_marks_the_stop();
void test_parse_signed_minutes_bounds_and_garbage();

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
void test_fetch_plausible_schedule_retries_past_wrong_service_day();
void test_fetch_plausible_schedule_accepts_first_good_answer_without_retrying();
void test_fetch_plausible_schedule_keeps_best_effort_when_every_answer_is_wrong();
void test_fetch_plausible_schedule_leaves_out_untouched_on_total_failure();
void test_fetch_plausible_schedule_stops_on_a_transport_failure();
void test_fetch_plausible_schedule_still_retries_a_backend_that_answers();
void test_poll_bus_stops_marks_wrong_day_schedule_as_suspect();
void test_poll_bus_stops_truncated_tripupdates_is_reported();
void test_poll_bus_stops_invalid_protobuf_is_reported();
void test_poll_bus_stops_subway_schedule_failure_is_local_to_that_stop();
void test_poll_bus_stops_501_with_a_valid_body_still_succeeds();
void test_poll_bus_stops_valid_empty_feed_succeeds();
void test_poll_rail_stops_one_failed_station_is_local_to_that_station();
void test_poll_rail_stops_malformed_json_is_a_failure();
void test_poll_buffers_keep_their_capacity_across_cycles();
void test_poll_buffers_do_not_change_what_a_cycle_produces();
void test_poll_buffers_return_an_oversized_body_buffer();
void test_a_poll_cycle_never_fetches_alerts();
void test_poll_buffers_remove_the_large_contiguous_requests();
void test_poll_buffers_keep_the_typed_blocks_across_cycles();
void test_poll_buffers_size_retention_from_the_config();
void test_the_scratch_reservation_ratchets_instead_of_churning();
void test_the_ratchet_waits_for_a_heap_that_can_spare_it();
void test_the_scratch_high_water_is_recorded();
void test_poll_buffers_hand_the_same_storage_to_the_indego_scanner();

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
  RUN_TEST(test_gtfsrt_retention_cap_holds_at_the_cap_with_500_matching_entities);
  RUN_TEST(test_gtfsrt_retention_global_cap_across_many_stops);
  RUN_TEST(test_gtfsrt_long_identifiers_are_truncated);
  RUN_TEST(test_gtfsrt_complete_body_reports_complete);
  RUN_TEST(test_gtfsrt_truncated_body_reports_truncated);
  RUN_TEST(test_gtfsrt_empty_body_is_complete_not_truncated);
  RUN_TEST(test_gtfsrt_malformed_entity_is_counted_not_fatal);
  RUN_TEST(test_gtfsrt_trip_level_canceled_is_surfaced);
  RUN_TEST(test_gtfsrt_fixture_updates_carry_the_header_timestamp);
  RUN_TEST(test_gtfsrt_reset_decodes_a_second_feed_identically);
  RUN_TEST(test_gtfsrt_reset_clears_a_half_parsed_feed);
  RUN_TEST(test_gtfsrt_borrowed_retention_block_is_never_reallocated);
  RUN_TEST(test_gtfsrt_retention_cap_can_be_sized_below_the_default);
  RUN_TEST(test_gtfsrt_reset_grows_the_entity_cap_when_asked);

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
  RUN_TEST(test_parse_int_bounded_rejects_overlong_digit_runs);
  RUN_TEST(test_parse_int_strict_rejects_trailing_garbage);
  RUN_TEST(test_time_parsers_reject_overlong_fields);

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
  RUN_TEST(test_parse_bus_schedules_caps_entries_and_keeps_the_nearest);
  RUN_TEST(test_parse_transitview_caps_vehicles);
  RUN_TEST(test_parse_transitview_filter_keeps_only_the_wanted_trips);
  RUN_TEST(test_parse_transitview_default_filter_keeps_every_vehicle);
  RUN_TEST(test_parse_transitview_a_filtered_out_vehicle_is_not_a_drop);
  RUN_TEST(test_parse_transitview_the_cap_counts_only_vehicles_the_filter_wanted);
  RUN_TEST(test_transitview_cap_is_sixteen_slots);
  RUN_TEST(test_transitview_trip_filter_changes_nothing_the_merge_reads);
  RUN_TEST(test_parse_transitview_truncates_long_identifiers);
  RUN_TEST(test_parse_transitview_absurd_numbers_do_not_overflow);
  RUN_TEST(test_rail_line_lookup_by_code_or_display_name);
  RUN_TEST(test_first_upcoming_schedule_time_matches_the_parser);
  RUN_TEST(test_first_upcoming_schedule_time_skips_entries_already_past);
  RUN_TEST(test_first_upcoming_schedule_time_reads_the_wrong_service_day_fixture);
  RUN_TEST(test_first_upcoming_schedule_time_tolerates_junk_and_truncation);

  RUN_TEST(test_merge_stop_joins_rt_and_tv_by_trip_id);
  RUN_TEST(test_merge_stop_drops_stale_arrivals);
  RUN_TEST(test_merge_stop_subway_schedule_only);
  RUN_TEST(test_merge_rail_direction_filter_and_status_mapping);
  RUN_TEST(test_merge_rail_on_time_status);
  RUN_TEST(test_merge_rail_line_filter);
  RUN_TEST(test_merge_rail_unrecognized_line_code_matches_nothing);
  RUN_TEST(test_merge_rail_no_direction_filter_returns_both);
  RUN_TEST(test_merge_stop_dedupes_scheduled_rows_by_trip_id);
  RUN_TEST(test_merge_stop_ignores_other_route_schedule_entries);
  RUN_TEST(test_merge_stop_live_arrival_does_not_match_other_route_schedule);
  RUN_TEST(test_sched_route_matches_is_case_insensitive_for_bus);
  RUN_TEST(test_merge_stop_subway_route_alias_from_fixture);
  RUN_TEST(test_sched_route_matches_unknown_subway_accepts_any);
  RUN_TEST(test_merge_stop_keeps_matched_static_trip_id);
  RUN_TEST(test_merge_stop_skipped_without_time_is_shown_and_consumes_schedule);
  RUN_TEST(test_merge_stop_skipped_with_time_keeps_prediction);
  RUN_TEST(test_merge_stop_canceled_trip_suppresses_its_schedule_row);
  RUN_TEST(test_merge_stop_no_data_leaves_the_schedule_row);
  RUN_TEST(test_merge_stop_plain_schedule_row_is_distinct);
  RUN_TEST(test_merge_stop_fresh_feed_is_live);
  RUN_TEST(test_merge_stop_old_feed_is_stale_with_no_live_rows);
  RUN_TEST(test_merge_stop_future_dated_feed_is_stale);
  RUN_TEST(test_merge_stop_missing_feed_timestamp_keeps_live_rows);
  RUN_TEST(test_merge_stop_subway_schedule_failure_is_unavailable);
  RUN_TEST(test_merge_stop_live_failure_falls_back_to_schedule_only);
  RUN_TEST(test_merge_rail_accepts_line_display_name_in_config);
  RUN_TEST(test_merge_rail_failed_fetch_marks_the_stop);
  RUN_TEST(test_parse_signed_minutes_bounds_and_garbage);

  RUN_TEST(test_septa_url_builders);
  RUN_TEST(test_alert_route_id_for_bus_and_trolley);
  RUN_TEST(test_alert_route_id_for_subway_uses_rr_prefix);
  RUN_TEST(test_alert_route_id_for_rail_uses_lookup_table);
  RUN_TEST(test_alert_route_id_for_empty_route);
  RUN_TEST(test_poll_bus_stops_merges_both_configured_stops);
  RUN_TEST(test_poll_bus_stops_subway_is_schedule_only);
  RUN_TEST(test_poll_bus_stops_transport_failure_marks_snapshot);
  RUN_TEST(test_poll_rail_stops_merges_by_direction);
  RUN_TEST(test_fetch_plausible_schedule_retries_past_wrong_service_day);
  RUN_TEST(test_fetch_plausible_schedule_accepts_first_good_answer_without_retrying);
  RUN_TEST(test_fetch_plausible_schedule_keeps_best_effort_when_every_answer_is_wrong);
  RUN_TEST(test_fetch_plausible_schedule_leaves_out_untouched_on_total_failure);
  RUN_TEST(test_fetch_plausible_schedule_stops_on_a_transport_failure);
  RUN_TEST(test_fetch_plausible_schedule_still_retries_a_backend_that_answers);
  RUN_TEST(test_poll_bus_stops_marks_wrong_day_schedule_as_suspect);
  RUN_TEST(test_poll_bus_stops_truncated_tripupdates_is_reported);
  RUN_TEST(test_poll_bus_stops_invalid_protobuf_is_reported);
  RUN_TEST(test_poll_bus_stops_subway_schedule_failure_is_local_to_that_stop);
  RUN_TEST(test_poll_bus_stops_501_with_a_valid_body_still_succeeds);
  RUN_TEST(test_poll_bus_stops_valid_empty_feed_succeeds);
  RUN_TEST(test_poll_rail_stops_one_failed_station_is_local_to_that_station);
  RUN_TEST(test_poll_rail_stops_malformed_json_is_a_failure);

  RUN_TEST(test_poll_buffers_keep_their_capacity_across_cycles);
  RUN_TEST(test_poll_buffers_do_not_change_what_a_cycle_produces);
  RUN_TEST(test_poll_buffers_return_an_oversized_body_buffer);
  RUN_TEST(test_a_poll_cycle_never_fetches_alerts);
  RUN_TEST(test_poll_buffers_remove_the_large_contiguous_requests);
  RUN_TEST(test_poll_buffers_keep_the_typed_blocks_across_cycles);
  RUN_TEST(test_poll_buffers_size_retention_from_the_config);
  RUN_TEST(test_the_scratch_reservation_ratchets_instead_of_churning);
  RUN_TEST(test_the_ratchet_waits_for_a_heap_that_can_spare_it);
  RUN_TEST(test_the_scratch_high_water_is_recorded);
  RUN_TEST(test_poll_buffers_hand_the_same_storage_to_the_indego_scanner);

  return UNITY_END();
}
