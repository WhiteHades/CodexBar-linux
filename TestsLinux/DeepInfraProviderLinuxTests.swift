import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
import Testing
@testable import CodexBarCLI
@testable import CodexBarCore

struct DeepInfraProviderLinuxTests {
    @Test
    func `maps prepaid balance and monthly cents`() throws {
        let now = Date(timeIntervalSince1970: 1_700_000_000)
        let snapshot = try DeepInfraUsageFetcher._parseSnapshotForTesting(
            checklistData: Self.checklist(stripeBalance: -99.75, recent: 3.94, limit: 20),
            usageData: Self.usage(totalCostCents: 394),
            now: now)
        let usage = snapshot.toUsageSnapshot()

        #expect(abs(snapshot.availableBalanceUSD - 95.81) < 0.000_001)
        #expect(snapshot.amountOwedUSD == 0)
        #expect(snapshot.currentMonthCostUSD == 3.94)
        #expect(usage.primary?.usedPercent == 0)
        #expect(usage.primary?.resetDescription == "$95.81 available · $3.94 spent this month")
        #expect(usage.providerCost?.used == 3.94)
        #expect(usage.providerCost?.limit == 20)
        #expect(usage.identity?.providerID == .deepinfra)
        #expect(usage.dataConfidence == .exact)
    }

    @Test
    func `maps owed suspended and empty month states`() throws {
        let owed = try DeepInfraUsageFetcher._parseSnapshotForTesting(
            checklistData: Self.checklist(stripeBalance: 2.75, recent: 7, limit: -1),
            usageData: Self.usage(totalCostCents: 650))
            .toUsageSnapshot()
        #expect(owed.primary?.usedPercent == 100)
        #expect(owed.primary?.resetDescription == "$9.75 owed · $6.50 spent this month")
        #expect(owed.providerCost == nil)

        let suspended = try DeepInfraUsageFetcher._parseSnapshotForTesting(
            checklistData: Self.checklist(
                stripeBalance: -5,
                recent: 1,
                limit: nil,
                suspended: true,
                suspendReason: " Payment review "),
            usageData: Data("{\"months\":[]}".utf8))
            .toUsageSnapshot()
        #expect(suspended.primary?.usedPercent == 100)
        #expect(suspended.primary?.resetDescription ==
            "Suspended: Payment review · $4.00 available · $1.00 spent this month")
    }

    @Test
    func `fetches checklist then usage with bearer authentication`() async throws {
        let recorder = DeepInfraRequestRecorder()
        let transport = ProviderHTTPTransportHandler { request in
            await recorder.append(request)
            let data = request.url?.path == "/payment/checklist"
                ? Self.checklist(stripeBalance: -9, recent: 2, limit: 10)
                : Self.usage(totalCostCents: 150)
            return try Self.response(for: request, data: data, statusCode: 200)
        }

        let snapshot = try await DeepInfraUsageFetcher._fetchUsageForTesting(
            apiKey: " fixture-token ",
            transport: transport,
            now: Date(timeIntervalSince1970: 123))
        let requests = await recorder.values

        #expect(snapshot.availableBalanceUSD == 7)
        #expect(requests.map(\.url?.path) == ["/payment/checklist", "/payment/usage"])
        #expect(requests[0].url?.query == "compute_owed=true")
        #expect(requests[1].url?.query == "from=current")
        #expect(requests.allSatisfy { $0.value(forHTTPHeaderField: "Authorization") == "Bearer fixture-token" })
        #expect(requests.allSatisfy { $0.timeoutInterval == 30 })
    }

    @Test
    func `preserves cancellation and rejects invalid responses`() async {
        let cancelling = ProviderHTTPTransportHandler { _ in throw CancellationError() }
        await #expect(throws: CancellationError.self) {
            _ = try await DeepInfraUsageFetcher._fetchUsageForTesting(
                apiKey: "key",
                transport: cancelling)
        }

        let unauthorized = ProviderHTTPTransportHandler { request in
            try Self.response(for: request, data: Data(), statusCode: 401)
        }
        await #expect {
            _ = try await DeepInfraUsageFetcher._fetchUsageForTesting(
                apiKey: "key",
                transport: unauthorized)
        } throws: { error in
            guard case let DeepInfraUsageError.apiError(message) = error else { return false }
            return message.contains("401")
        }
    }

    @Test
    func `reads credential precedence and descriptor aliases`() throws {
        #expect(DeepInfraSettingsReader.apiKey(environment: [
            "DEEPINFRA_API_KEY": " 'primary' ",
            "DEEPINFRA_TOKEN": "alternate",
        ]) == "primary")
        #expect(DeepInfraSettingsReader.apiKey(environment: ["DEEPINFRA_TOKEN": " alternate "]) == "alternate")

        let descriptor = ProviderDescriptorRegistry.descriptor(for: .deepinfra)
        #expect(descriptor.metadata.displayName == "DeepInfra")
        #expect(descriptor.cli.name == "deepinfra")
        #expect(descriptor.cli.aliases == ["deep-infra", "di"])
        #expect(try #require(ProviderSelection(argument: "di")).asList == [.deepinfra])
    }

    @Test
    func `rejects malformed billing response`() {
        #expect {
            _ = try DeepInfraUsageFetcher._parseSnapshotForTesting(
                checklistData: Data("{}".utf8),
                usageData: Self.usage(totalCostCents: 100))
        } throws: { error in
            guard case DeepInfraUsageError.parseFailed = error else { return false }
            return true
        }
    }

    private static func checklist(
        stripeBalance: Double,
        recent: Double,
        limit: Double?,
        suspended: Bool = false,
        suspendReason: String? = nil) -> Data
    {
        let limitJSON = limit.map { Swift.String($0) } ?? "null"
        let reasonJSON = suspendReason.map { "\"\($0)\"" } ?? "null"
        return Data(
            """
            {
              "stripe_balance": \(stripeBalance),
              "recent": \(recent),
              "limit": \(limitJSON),
              "suspended": \(suspended),
              "suspend_reason": \(reasonJSON)
            }
            """.utf8)
    }

    private static func usage(totalCostCents: Double) -> Data {
        Data(
            """
            {
              "months": [{"period":"2026.07","items":[],"total_cost":\(totalCostCents)}],
              "initial_month":"2026.07"
            }
            """.utf8)
    }

    private static func response(
        for request: URLRequest,
        data: Data,
        statusCode: Int) throws -> (Data, URLResponse)
    {
        guard let url = request.url,
              let response = HTTPURLResponse(
                  url: url,
                  statusCode: statusCode,
                  httpVersion: "HTTP/1.1",
                  headerFields: ["Content-Type": "application/json"])
        else {
            throw URLError(.badServerResponse)
        }
        return (data, response)
    }
}

private actor DeepInfraRequestRecorder {
    private(set) var values: [URLRequest] = []

    func append(_ request: URLRequest) {
        self.values.append(request)
    }
}
