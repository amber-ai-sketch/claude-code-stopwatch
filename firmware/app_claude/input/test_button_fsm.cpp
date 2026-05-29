// Host-side smoke test for ButtonFsm. Build:
//   c++ -std=c++17 -I.. test_button_fsm.cpp button_fsm.cpp -o /tmp/btn_test
//   /tmp/btn_test
// Not part of the firmware build.
#include "button_fsm.h"
#include <stdio.h>
#include <assert.h>

using clawd_watch::ButtonFsm;
using clawd_watch::ButtonEvent;

static const char* ev_name(ButtonEvent e) {
    switch (e) {
        case ButtonEvent::None: return "None";
        case ButtonEvent::Single: return "Single";
        case ButtonEvent::Double: return "Double";
        case ButtonEvent::Long: return "Long";
    }
    return "?";
}

// Drive the FSM forward in 10ms ticks for `duration_ms` with the given
// `pressed` state. Captures the LAST non-None event. Returns it.
static ButtonEvent run(ButtonFsm& f, bool pressed, uint32_t& t, uint32_t duration_ms,
                       ButtonEvent* out_seen = nullptr) {
    ButtonEvent last_seen = ButtonEvent::None;
    for (uint32_t i = 0; i < duration_ms; i += 10) {
        ButtonEvent e = f.update(pressed, t);
        if (e != ButtonEvent::None) {
            last_seen = e;
            if (out_seen) *out_seen = e;
        }
        t += 10;
    }
    return last_seen;
}

int main() {
    int passed = 0, failed = 0;

    auto check = [&](const char* label, bool ok) {
        printf("  %s %s\n", ok ? "✓" : "✗", label);
        if (ok) passed++; else failed++;
    };

    // Test 1: short press → SINGLE after 300ms double window.
    {
        printf("Test 1: short press → SINGLE\n");
        ButtonFsm f("t1");
        uint32_t t = 0;
        // Press for 100ms (under 200ms single threshold).
        run(f, true,  t, 100);
        // Release.
        ButtonEvent first_rel = run(f, false, t, 50);  // 50ms after release, no event
        check("no immediate event on release", first_rel == ButtonEvent::None);
        // Wait past the double-click window (300ms total since release).
        ButtonEvent ev = ButtonEvent::None;
        run(f, false, t, 300, &ev);
        check("Single emitted after double window", ev == ButtonEvent::Single);
    }

    // Test 2: long press → LONG immediately at 500ms.
    {
        printf("Test 2: long press → LONG immediately\n");
        ButtonFsm f("t2");
        uint32_t t = 0;
        ButtonEvent ev = ButtonEvent::None;
        run(f, true, t, 510, &ev);
        check("Long emitted around 500ms", ev == ButtonEvent::Long);
        // Continued holding shouldn't emit more.
        ButtonEvent ev2 = ButtonEvent::None;
        run(f, true, t, 500, &ev2);
        check("no spurious event while held", ev2 == ButtonEvent::None);
        // Release returns to idle, no event.
        ButtonEvent ev3 = ButtonEvent::None;
        run(f, false, t, 100, &ev3);
        check("no event on release after Long", ev3 == ButtonEvent::None);
    }

    // Test 3: double click → DOUBLE.
    {
        printf("Test 3: double click → DOUBLE\n");
        ButtonFsm f("t3");
        uint32_t t = 0;
        run(f, true,  t, 100);  // press 1 (100ms)
        run(f, false, t, 100);  // release 1, gap 100ms
        ButtonEvent ev = ButtonEvent::None;
        run(f, true,  t, 50, &ev);  // press 2 starts
        check("Double emitted on second press", ev == ButtonEvent::Double);
        // No further event while still held.
        ButtonEvent ev2 = ButtonEvent::None;
        run(f, true, t, 200, &ev2);
        check("no spurious after Double", ev2 == ButtonEvent::None);
        run(f, false, t, 100);  // release 2
        // After settling, a fresh single click should still work.
        ButtonEvent ev3 = ButtonEvent::None;
        run(f, true,  t, 100);
        run(f, false, t, 50);
        run(f, false, t, 300, &ev3);
        check("FSM resets to Idle correctly", ev3 == ButtonEvent::Single);
    }

    // Test 4: medium press (dead zone, 200-500ms) → no event.
    {
        printf("Test 4: medium press (dead zone) → no event\n");
        ButtonFsm f("t4");
        uint32_t t = 0;
        ButtonEvent ev = ButtonEvent::None;
        run(f, true,  t, 350, &ev);  // 350ms: > Single, < Long
        check("no event during medium hold", ev == ButtonEvent::None);
        run(f, false, t, 500, &ev);  // wait long enough that Single window expires
        check("no event after release", ev == ButtonEvent::None);
    }

    // Test 5: bouncy press → only one event.
    {
        printf("Test 5: bouncy press (2ms bounce) → one Single\n");
        ButtonFsm f("t5");
        uint32_t t = 0;
        // Simulate 5 quick bounces over 5ms (much faster than debounce).
        for (int i = 0; i < 5; i++) {
            f.update(i % 2 == 0, t);
            t += 1;  // 1ms ticks
        }
        // Now hold steadily for 100ms.
        for (uint32_t i = 0; i < 100; i++) {
            f.update(true, t);
            t += 1;
        }
        // Release.
        for (uint32_t i = 0; i < 50; i++) {
            f.update(false, t);
            t += 1;
        }
        // Wait past double window.
        ButtonEvent ev = ButtonEvent::None;
        for (uint32_t i = 0; i < 350; i++) {
            ButtonEvent e = f.update(false, t);
            if (e != ButtonEvent::None) ev = e;
            t += 1;
        }
        check("Single (bounce filtered)", ev == ButtonEvent::Single);
    }

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
