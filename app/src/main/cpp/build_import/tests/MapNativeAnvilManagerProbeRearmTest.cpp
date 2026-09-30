#include "../MapNativeAnvilManagerProbe.h"

#include <cassert>

int main() {
    using build_import::CanSequentiallyRearmMapNativeAnvilProbe;
    using build_import::IsFreshMapNativeAnvilProbeTicket;
    using build_import::MapNativeAnvilRearmEvidence;
    using build_import::MapNativeAnvilWindowFields;
    using build_import::MatchMapNativeAnvilWindowFields;

    MapNativeAnvilRearmEvidence ready{};
    ready.explicitly_disarmed = true;
    ready.manager_vtable_matches = true;
    ready.screen_vtable_matches = true;
    ready.screen_manager_matches = true;
    ready.manager_constructors = 1;
    ready.screen_constructors = 1;
    ready.manager_destructors = 1;
    ready.screen_destructors = 1;
    ready.manager_complete_destructor_epilogues = 1;
    ready.screen_complete_destructor_epilogues = 1;
    // A complete-destructor path need not invoke the deleting wrapper.
    assert(CanSequentiallyRearmMapNativeAnvilProbe(ready));

    auto evidence = ready;
    evidence.manager_delete_completions = 1;
    evidence.screen_delete_completions = 1;
    assert(CanSequentiallyRearmMapNativeAnvilProbe(evidence));

    evidence.explicitly_disarmed = false;
    assert(!CanSequentiallyRearmMapNativeAnvilProbe(evidence));
    evidence = ready;
    evidence.manager_complete_destructor_epilogues = 0;
    assert(!CanSequentiallyRearmMapNativeAnvilProbe(evidence));
    evidence = ready;
    evidence.screen_complete_destructor_epilogues = 0;
    assert(!CanSequentiallyRearmMapNativeAnvilProbe(evidence));
    evidence = ready;
    evidence.manager_delete_completions = 2;
    assert(!CanSequentiallyRearmMapNativeAnvilProbe(evidence));
    evidence = ready;
    evidence.screen_delete_completions = 2;
    assert(!CanSequentiallyRearmMapNativeAnvilProbe(evidence));
    evidence = ready;
    evidence.callbacks_in_flight = 1;
    assert(!CanSequentiallyRearmMapNativeAnvilProbe(evidence));
    evidence = ready;
    evidence.unowned_constructor_observed = true;
    assert(!CanSequentiallyRearmMapNativeAnvilProbe(evidence));
    evidence = ready;
    evidence.ambiguous = true;
    assert(!CanSequentiallyRearmMapNativeAnvilProbe(evidence));
    evidence = ready;
    evidence.manager_constructors = 2;
    assert(!CanSequentiallyRearmMapNativeAnvilProbe(evidence));
    evidence = ready;
    evidence.screen_manager_matches = false;
    assert(!CanSequentiallyRearmMapNativeAnvilProbe(evidence));
    assert(IsFreshMapNativeAnvilProbeTicket(0, 1));
    assert(IsFreshMapNativeAnvilProbeTicket(37, 38));
    assert(!IsFreshMapNativeAnvilProbeTicket(37, 37));
    assert(!IsFreshMapNativeAnvilProbeTicket(37, 12));
    assert(!IsFreshMapNativeAnvilProbeTicket(0, 0));

    MapNativeAnvilWindowFields fields{};
    fields.capture_x = fields.screen_x = fields.controller_x = 14;
    fields.capture_y = fields.screen_y = fields.controller_y = 65;
    fields.capture_z = fields.screen_z = fields.controller_z = -8;
    fields.capture_id = fields.controller_id = 7U;
    fields.capture_type = fields.controller_type = 5U;
    fields.screen_target_kind = 1U;
    assert(MatchMapNativeAnvilWindowFields(fields));
    fields.controller_id = 0xFFU;
    assert(!MatchMapNativeAnvilWindowFields(fields));
    fields.controller_id = 7U;
    fields.screen_z = -9;
    assert(!MatchMapNativeAnvilWindowFields(fields));
    fields.screen_z = -8;
    fields.controller_type = 0U;
    assert(!MatchMapNativeAnvilWindowFields(fields));
    fields.controller_type = 5U;
    fields.capture_id = 0U;
    assert(!MatchMapNativeAnvilWindowFields(fields));
    return 0;
}
