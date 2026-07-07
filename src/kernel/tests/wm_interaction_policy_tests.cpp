#include <kernel/ktest.h>
#include <wm/interaction_policy.h>
#include <drivers/class/hid/ps2_mouse.h>

KTEST(wm_policy_submit_when_queue_has_capacity)
{
    wm::PresentPolicyInput in = {};
    in.pending = 0;
    in.queue_limit = 2;
    in.strict_sync = false;
    in.interactive = true;
    in.copy_path = false;
    in.active_manipulation = true;

    KTEST_EXPECT_EQ(wm::choose_present_policy(in), wm::PresentPolicyDecision::Submit);
}

KTEST(wm_policy_wait_on_full_queue_for_strict_sync)
{
    wm::PresentPolicyInput in = {};
    in.pending = 2;
    in.queue_limit = 2;
    in.strict_sync = true;
    in.interactive = true;
    in.copy_path = false;
    in.active_manipulation = true;

    KTEST_EXPECT_EQ(wm::choose_present_policy(in), wm::PresentPolicyDecision::Wait);
}

KTEST(wm_policy_wait_on_full_queue_for_non_interactive)
{
    wm::PresentPolicyInput in = {};
    in.pending = 2;
    in.queue_limit = 2;
    in.strict_sync = false;
    in.interactive = false;
    in.copy_path = false;
    in.active_manipulation = true;

    KTEST_EXPECT_EQ(wm::choose_present_policy(in), wm::PresentPolicyDecision::Wait);
}

KTEST(wm_policy_skip_on_full_queue_for_copy_path_during_active_manipulation)
{
    wm::PresentPolicyInput in = {};
    in.pending = 1;
    in.queue_limit = 1;
    in.strict_sync = false;
    in.interactive = true;
    in.copy_path = true;
    in.active_manipulation = true;

    KTEST_EXPECT_EQ(wm::choose_present_policy(in), wm::PresentPolicyDecision::Skip);
}

KTEST(wm_policy_wait_on_full_queue_for_copy_path_without_active_manipulation)
{
    wm::PresentPolicyInput in = {};
    in.pending = 1;
    in.queue_limit = 1;
    in.strict_sync = false;
    in.interactive = true;
    in.copy_path = true;
    in.active_manipulation = false;

    KTEST_EXPECT_EQ(wm::choose_present_policy(in), wm::PresentPolicyDecision::Wait);
}

KTEST(wm_policy_skip_on_full_queue_during_active_manipulation)
{
    wm::PresentPolicyInput in = {};
    in.pending = 2;
    in.queue_limit = 2;
    in.strict_sync = false;
    in.interactive = true;
    in.copy_path = false;
    in.active_manipulation = true;

    KTEST_EXPECT_EQ(wm::choose_present_policy(in), wm::PresentPolicyDecision::Skip);
}

KTEST(wm_policy_wait_on_full_queue_without_active_manipulation)
{
    wm::PresentPolicyInput in = {};
    in.pending = 2;
    in.queue_limit = 2;
    in.strict_sync = false;
    in.interactive = true;
    in.copy_path = false;
    in.active_manipulation = false;

    KTEST_EXPECT_EQ(wm::choose_present_policy(in), wm::PresentPolicyDecision::Wait);
}

KTEST(wm_constants_match_current_behavior)
{
    KTEST_EXPECT_EQ(wm::dirty_collapse_ratio_num(), 5u);
    KTEST_EXPECT_EQ(wm::dirty_collapse_ratio_den(), 4u);
    KTEST_EXPECT_EQ(wm::interactive_dirty_collapse_limit(), 10);
    KTEST_EXPECT_EQ(wm::non_interactive_dirty_collapse_limit(), 6);
}

KTEST(wm_pending_presents_saturates_at_zero)
{
    KTEST_EXPECT_EQ(wm::pending_presents(5u, 5u), 0u);
    KTEST_EXPECT_EQ(wm::pending_presents(4u, 7u), 0u);
    KTEST_EXPECT_EQ(wm::pending_presents(9u, 6u), 3u);
}

KTEST(wm_completion_target_for_available_slot_tracks_queue_depth)
{
    KTEST_EXPECT_EQ(wm::completion_target_for_available_slot(1u, 1u), 1u);
    KTEST_EXPECT_EQ(wm::completion_target_for_available_slot(2u, 2u), 1u);
    KTEST_EXPECT_EQ(wm::completion_target_for_available_slot(5u, 2u), 4u);
    KTEST_EXPECT_EQ(wm::completion_target_for_available_slot(1u, 3u), 0u);
}

KTEST(wm_normalize_dirty_rects_collapses_interactive_bounds)
{
    wm::DirtyRect rects[11] = {
        {0, 0, 20, 20},   {30, 0, 20, 20},  {60, 0, 20, 20},  {90, 0, 20, 20},  {120, 0, 20, 20}, {150, 0, 20, 20},
        {180, 0, 20, 20}, {210, 0, 20, 20}, {240, 0, 20, 20}, {270, 0, 20, 20}, {300, 0, 20, 20},
    };
    int count = 11;

    wm::normalize_dirty_rects(rects, &count, 640, 480, true);

    KTEST_EXPECT_EQ(count, 1);
    KTEST_EXPECT_EQ(rects[0].x, 0);
    KTEST_EXPECT_EQ(rects[0].y, 0);
    KTEST_EXPECT_EQ(rects[0].w, 320);
    KTEST_EXPECT_EQ(rects[0].h, 20);
}

KTEST(wm_enqueue_damage_rect_collapses_full_screen)
{
    wm::DirtyRect rects[4] = {{10, 10, 20, 20}};
    int count = 1;

    wm::enqueue_damage_rect(rects, &count, 4, 800, 600, {0, 0, 800, 600});

    KTEST_EXPECT_EQ(count, 1);
    KTEST_EXPECT_EQ(rects[0].x, 0);
    KTEST_EXPECT_EQ(rects[0].y, 0);
    KTEST_EXPECT_EQ(rects[0].w, 800);
    KTEST_EXPECT_EQ(rects[0].h, 600);
}

KTEST(wm_exposed_transition_returns_full_old_rect_when_disjoint)
{
    wm::DirtyRect old_outer = {10, 10, 40, 30};
    wm::DirtyRect new_outer = {100, 100, 20, 20};

    wm::ExposedTransitionDamage damage = wm::compute_exposed_transition_damage(old_outer, new_outer);
    KTEST_EXPECT_EQ(damage.count, 1);
    KTEST_EXPECT_EQ(damage.rects[0].x, 10);
    KTEST_EXPECT_EQ(damage.rects[0].y, 10);
    KTEST_EXPECT_EQ(damage.rects[0].w, 40);
    KTEST_EXPECT_EQ(damage.rects[0].h, 30);
}

KTEST(wm_exposed_transition_returns_no_damage_when_old_is_fully_covered)
{
    wm::DirtyRect old_outer = {20, 20, 30, 30};
    wm::DirtyRect new_outer = {10, 10, 80, 80};

    wm::ExposedTransitionDamage damage = wm::compute_exposed_transition_damage(old_outer, new_outer);
    KTEST_EXPECT_EQ(damage.count, 0);
}

KTEST(wm_exposed_transition_reports_old_only_strips_for_partial_overlap)
{
    wm::DirtyRect old_outer = {10, 10, 100, 60};
    wm::DirtyRect new_outer = {50, 20, 80, 80};

    wm::ExposedTransitionDamage damage = wm::compute_exposed_transition_damage(old_outer, new_outer);
    KTEST_EXPECT_EQ(damage.count, 2);

    KTEST_EXPECT_EQ(damage.rects[0].x, 10);
    KTEST_EXPECT_EQ(damage.rects[0].y, 10);
    KTEST_EXPECT_EQ(damage.rects[0].w, 100);
    KTEST_EXPECT_EQ(damage.rects[0].h, 10);

    KTEST_EXPECT_EQ(damage.rects[1].x, 10);
    KTEST_EXPECT_EQ(damage.rects[1].y, 20);
    KTEST_EXPECT_EQ(damage.rects[1].w, 40);
    KTEST_EXPECT_EQ(damage.rects[1].h, 50);
}

KTEST(wm_exposed_transition_reports_uncovered_strip_for_horizontal_move)
{
    wm::DirtyRect old_outer = {100, 100, 120, 80};
    wm::DirtyRect new_outer = {140, 100, 120, 80};

    wm::ExposedTransitionDamage damage = wm::compute_exposed_transition_damage(old_outer, new_outer);
    KTEST_EXPECT_EQ(damage.count, 1);
    KTEST_EXPECT_EQ(damage.rects[0].x, 100);
    KTEST_EXPECT_EQ(damage.rects[0].y, 100);
    KTEST_EXPECT_EQ(damage.rects[0].w, 40);
    KTEST_EXPECT_EQ(damage.rects[0].h, 80);
}

KTEST(ps2_mouse_thread_safety)
{
    MouseState state = ps2_mouse_get_state();
    KTEST_EXPECT(state.x >= 0);
    KTEST_EXPECT(state.y >= 0);
}
