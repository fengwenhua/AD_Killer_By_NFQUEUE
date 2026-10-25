#include "work_budget.h"
#include <cassert>
#include <iostream>
#include <type_traits>

int main() {
    static_assert(!std::is_copy_constructible<WorkBudget>::value, "scopes cannot be copied");
    static_assert(!std::is_copy_constructible<WorkBudget::Reservation>::value, "reservations cannot be copied");
    assert(!WorkBudget::active && !WorkBudget::expired());
    assert(WorkBudget::remaining_ms(17) == 17 && WorkBudget::remaining_bytes(42) == 42);
    {
        WorkBudget task(10000, 100);
        {
            WorkBudget source(20000, 60);
            assert(WorkBudget::remaining_ms(30000) <= 10000);
            WorkBudget::Reservation first(80);
            assert(first.amount() == 60 && WorkBudget::remaining_bytes(1000) == 0);
            first.settle(61); // malformed accounting must not release anything
            assert(WorkBudget::remaining_bytes(1000) == 0);
            first.settle(30);
            first.settle(0); // duplicate settlement must not double-refund
            assert(WorkBudget::remaining_bytes(1000) == 30);
            { WorkBudget::Reservation unknown(20); } // unknown child retains its charge
            assert(WorkBudget::remaining_bytes(1000) == 10);
        }
        assert(WorkBudget::remaining_bytes(1000) == 50);
        {
            WorkBudget retry(30000, 1000);
            WorkBudget::Reservation rest(1000);
            assert(rest.amount() == 50 && WorkBudget::remaining_bytes(1) == 0);
            rest.settle(0);
        }
        assert(WorkBudget::remaining_bytes(1000) == 50);
        { WorkBudget expired(0, 100); assert(WorkBudget::expired() && WorkBudget::remaining_ms(100) == 0); }
        assert(!WorkBudget::expired());
    }
    {
        WorkBudget task(0, 0), nested(UINT64_MAX, UINT64_MAX);
        assert(WorkBudget::expired() && WorkBudget::remaining_bytes(1) == 0);
        WorkBudget::Reservation none(UINT64_MAX);
        assert(none.amount() == 0);
    }
    assert(!WorkBudget::active && WorkBudget::remaining_bytes(9) == 9);
    std::cout << "work budgets: nested monotonic deadlines, cumulative caps, conservative reservations and single refunds passed\n";
}
