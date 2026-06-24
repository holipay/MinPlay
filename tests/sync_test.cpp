#include "minunit.h"
#include "../src/sync/sync_context.h"

static void test_sync_render_in_window() {
    SyncContext sc(0.040);
    // diff < sync_window → Render
    SyncDecision d = sc.Decide(10.0, 9.97);
    MU_CHECK_EQ((int)d.action, (int)SyncAction::Render);
}

static void test_sync_drop_behind_threshold() {
    SyncContext sc(0.040);
    // diff < -drop_threshold (2.0) → Drop
    SyncDecision d = sc.Decide(10.0, 12.5);
    MU_CHECK_EQ((int)d.action, (int)SyncAction::Drop);
}

static void test_sync_wait_ahead_small() {
    SyncContext sc(0.040);
    // diff < wait_limit (0.200) → Wait
    SyncDecision d = sc.Decide(10.0, 9.85);
    MU_CHECK_EQ((int)d.action, (int)SyncAction::Wait);
    MU_CHECK(d.wait_ms > 0);
    // wait_ms should be ~150ms
    MU_CHECK(d.wait_ms >= 140 && d.wait_ms <= 160);
}

static void test_sync_wait_ahead_large() {
    SyncContext sc(0.040);
    // diff > wait_limit (0.500) → Wait capped at wait_limit
    SyncDecision d = sc.Decide(10.0, 9.5);
    MU_CHECK_EQ((int)d.action, (int)SyncAction::Wait);
    MU_CHECK(d.wait_ms <= 500);
}

static void test_sync_render_within_window_small() {
    SyncContext sc(0.040);
    // diff = 0.001 < sync_window (0.040) → Render
    SyncDecision d = sc.Decide(10.0, 9.999);
    MU_CHECK_EQ((int)d.action, (int)SyncAction::Render);
}

static void test_sync_threshold_edge() {
    SyncContext sc(0.040);
    // exactly at drop threshold (0.150)
    SyncDecision d = sc.Decide(10.0, 10.151);
    MU_CHECK_EQ((int)d.action, (int)SyncAction::Drop);
    d = sc.Decide(10.0, 10.149);
    MU_CHECK_EQ((int)d.action, (int)SyncAction::Render);
}

static void test_sync_seek_resets_stats() {
    SyncContext sc(0.040);
    sc.Decide(10.0, 10.3);  // drop
    sc.Decide(10.0, 10.3);  // drop
    sc.Seek();
    // No direct way to check stats reset without getter, but at minimum no crash
    SyncDecision d = sc.Decide(10.0, 9.97);
    MU_CHECK_EQ((int)d.action, (int)SyncAction::Render);
}

void sync_suite() {
    MU_RUN_TEST(test_sync_render_in_window);
    MU_RUN_TEST(test_sync_drop_behind_threshold);
    MU_RUN_TEST(test_sync_wait_ahead_small);
    MU_RUN_TEST(test_sync_wait_ahead_large);
    MU_RUN_TEST(test_sync_render_within_window_small);
    MU_RUN_TEST(test_sync_threshold_edge);
    MU_RUN_TEST(test_sync_seek_resets_stats);
}
