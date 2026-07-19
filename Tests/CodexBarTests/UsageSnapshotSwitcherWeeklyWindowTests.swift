import CodexBarCore
import Testing

struct UsageSnapshotSwitcherWeeklyWindowTests {
    @Test
    func `switcher weekly prefers secondary window when primary and secondary are both present`() {
        let snapshot = UsageSnapshot(
            primary: RateWindow(usedPercent: 80, windowMinutes: 5 * 60, resetsAt: nil, resetDescription: nil),
            secondary: RateWindow(usedPercent: 20, windowMinutes: 7 * 24 * 60, resetsAt: nil, resetDescription: nil),
            updatedAt: Date())

        let window = snapshot.switcherWeeklyWindow(for: .claude, showUsed: false)

        #expect(window?.windowMinutes == 7 * 24 * 60)
        #expect(window?.usedPercent == 20)
    }

    @Test
    func `switcher weekly falls back to primary window when secondary is absent`() {
        let primary = RateWindow(usedPercent: 42, windowMinutes: 5 * 60, resetsAt: nil, resetDescription: nil)
        let snapshot = UsageSnapshot(
            primary: primary,
            updatedAt: Date())

        let window = snapshot.switcherWeeklyWindow(for: .claude, showUsed: false)

        #expect(window == primary)
    }
}
